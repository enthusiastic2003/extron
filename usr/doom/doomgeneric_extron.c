/*
 * doomgeneric platform layer for Extron.
 *
 * The five DG_* functions doomgeneric asks a port to supply, and
 * nothing else — no Extron-specific syscalls were added for this.
 *
 *   - the framebuffer is a real device, /dev/fb0: opened and mmap()'d
 *     here like any other program would, so DG_DrawFrame is plain
 *     stores to memory with no svc in the frame loop once mapped
 *   - a keystroke ring is still mapped by exec (PROC_MAP_FRAMEBUFFER —
 *     see kernel/proc/exec.c), so DG_GetKey can poll without blocking,
 *     which SYS_READ cannot do
 */
#include "doomkeys.h"
#include "doomgeneric.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "../include/extron/syscall.h"
#include "../include/extron/fb.h"

/* Geometry read once at DG_Init(); base is the mmap()'d pixel pointer. */
static struct {
    uint64_t base;
    struct extron_fb_geometry geo;
} fb;

static uint64_t extron_uptime_ms(void) {
    return (uint64_t)__syscall0(SYS_UPTIME_MS);
}

/* Where DOOM's 640x400 image sits inside whatever the display actually
 * is. Computed from the reported geometry, not assumed. */
static uint32_t origin_x, origin_y;

/* DOOM always composes pixels as 0xAARRGGBB. If the firmware handed us a
 * framebuffer in the other byte order, DG_DrawFrame has to exchange red
 * and blue itself rather than memcpy. fb.c asks for the matching order,
 * so this should stay 0 — but "should" is not a reason to render
 * garbage if it doesn't. */
static int swap_rb;

#define KEYQUEUE_SIZE 32
static uint16_t key_queue[KEYQUEUE_SIZE];
static unsigned key_write, key_read;

/*
 * A serial terminal sends a byte when a key is pressed and NOTHING when
 * it is released — but DOOM needs both edges, or a movement key sticks
 * down forever and the player walks into a wall until the heat death of
 * the universe.
 *
 * So releases are synthesised: a key is marked down when its byte
 * arrives and released once no further byte for it has appeared for
 * KEY_HOLD_MS. Terminals auto-repeat a held key, so holding produces a
 * stream of bytes that keeps pushing the deadline out, and letting go
 * lets it expire. The threshold has to sit above the terminal's repeat
 * interval (typically ~30ms after an initial ~500ms delay) and below
 * anything a player would notice, which is a wide target.
 */
/*
 * Two thresholds, because a terminal's auto-repeat has two phases: a
 * long initial delay (commonly ~500ms) and then a fast stream (~30ms).
 *
 * A single threshold cannot serve both. Set it short and a genuinely
 * held key is released during the initial delay and re-pressed when the
 * repeats start, which stutters. Set it long and every tap lingers.
 * So the first byte for a key arms the long deadline, and the first
 * repeat — proof the key is actually being held — switches it to the
 * short one.
 */
#define KEY_INITIAL_HOLD_MS 600
#define KEY_REPEAT_HOLD_MS  120
#define MAX_HELD            8

static struct {
    unsigned char key;
    uint32_t      last_ms;
    int           repeating;   /* seen a second byte for this key */
} held[MAX_HELD];

static void queue_key(int pressed, unsigned char key) {
    if (!key) {
        return;
    }
    key_queue[key_write] = (uint16_t)((pressed << 8) | key);
    key_write = (key_write + 1) % KEYQUEUE_SIZE;
}

/* Serial gives us ASCII, not scancodes — so this maps characters rather
 * than the scancode tables the other doomgeneric ports use. Arrow keys
 * arrive as ANSI escape sequences (ESC [ A) which would need a parser,
 * so WASD is offered alongside; ESC itself is passed through when it is
 * not the start of a sequence. */
static unsigned char to_doom_key(int c) {
    switch (c) {
        case '\r': case '\n': return KEY_ENTER;
        case 27:              return KEY_ESCAPE;
        case 'w': case 'W':   return KEY_UPARROW;
        case 's': case 'S':   return KEY_DOWNARROW;
        case 'a': case 'A':   return KEY_LEFTARROW;
        case 'd': case 'D':   return KEY_RIGHTARROW;
        case ' ':             return KEY_USE;
        case 'f': case 'F':   return KEY_FIRE;
        case '\t':            return KEY_TAB;
        case 'y': case 'Y':   return 'y';
        case 'n': case 'N':   return 'n';
        case '1': return '1'; case '2': return '2'; case '3': return '3';
        case '4': return '4'; case '5': return '5'; case '6': return '6';
        case '7': return '7';
        case 127: case 8:     return KEY_BACKSPACE;
        default:              return 0;
    }
}

static void note_press(unsigned char key, uint32_t now) {
    for (int i = 0; i < MAX_HELD; i++) {
        if (held[i].key == key) {
            /* A repeat: the key is genuinely down, so the short deadline
             * applies from here on. */
            held[i].last_ms   = now;
            held[i].repeating = 1;
            return;
        }
    }
    for (int i = 0; i < MAX_HELD; i++) {
        if (!held[i].key) {
            held[i].key       = key;
            held[i].last_ms   = now;
            held[i].repeating = 0;
            queue_key(1, key);
            return;
        }
    }
}

static void expire_holds(uint32_t now) {
    for (int i = 0; i < MAX_HELD; i++) {
        uint32_t limit = held[i].repeating ? KEY_REPEAT_HOLD_MS
                                           : KEY_INITIAL_HOLD_MS;
        if (held[i].key && (now - held[i].last_ms) > limit) {
            queue_key(0, held[i].key);
            held[i].key = 0;
        }
    }
}

static struct extron_input_ring *input_ring;

static void poll_input(void) {
    if (!input_ring) return;
    uint32_t now = (uint32_t)extron_uptime_ms();
    int c;
    while ((c = extron_input_getc(input_ring)) >= 0) {
        unsigned char key = to_doom_key(c);
        if (key) {
            note_press(key, now);
        }
    }
    expire_holds(now);
}

static void init_input_ring(void) {
    int fd = open(EXTRON_INPUT_PATH, O_RDWR);
    if (fd < 0) {
        printf("DOOM: could not open %s, no keyboard input\n", EXTRON_INPUT_PATH);
        return;
    }
    void *mapped = mmap(NULL, sizeof(struct extron_input_ring),
                        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (mapped == MAP_FAILED) {
        printf("DOOM: failed to mmap %s, no keyboard input\n", EXTRON_INPUT_PATH);
        return;
    }
    input_ring = mapped;
}

void DG_Init(void) {
    init_input_ring();

    int fd = open(EXTRON_FB_PATH, O_RDWR);
    if (fd < 0) {
        printf("DOOM: could not open %s\n", EXTRON_FB_PATH);
        return;
    }
    if (read(fd, &fb.geo, sizeof(fb.geo)) != (long)sizeof(fb.geo)
            || !fb.geo.width || !fb.geo.pitch) {
        printf("DOOM: no framebuffer mapped\n");
        close(fd);
        return;
    }
    void *mapped = mmap(NULL, fb.geo.size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
    close(fd); /* the mapping outlives the fd, same as any other mmap() */
    if (mapped == MAP_FAILED) {
        printf("DOOM: failed to mmap %s\n", EXTRON_FB_PATH);
        fb.geo.pitch = 0; /* sentinel DG_DrawFrame checks below */
        return;
    }
    fb.base = (uint64_t)mapped;

    /* Centre DOOM's image on whatever the display turned out to be. It
     * is 640x400 against a 640x480 panel here, but neither number is
     * assumed — a smaller display just gets clipped by DG_DrawFrame. */
    origin_x = fb.geo.width  > DOOMGENERIC_RESX ? (fb.geo.width  - DOOMGENERIC_RESX) / 2 : 0;
    origin_y = fb.geo.height > DOOMGENERIC_RESY ? (fb.geo.height - DOOMGENERIC_RESY) / 2 : 0;

    /* rgb_order == 1 means byte 0 is red, i.e. 0xAABBGGRR — the opposite
     * of what DOOM writes. */
    swap_rb = (fb.geo.rgb_order != 0);

    /* Black the whole panel once, so the letterbox around the image
     * isn't left showing whatever was on screen before. */
    for (uint32_t y = 0; y < fb.geo.height; y++) {
        volatile uint32_t *row =
            (volatile uint32_t *)(fb.base + (uint64_t)y * fb.geo.pitch);
        for (uint32_t x = 0; x < fb.geo.width; x++) {
            row[x] = 0xFF000000u;
        }
    }

    printf("DOOM: %ux%u display, %ux%u image at +%u+%u%s\n",
           fb.geo.width, fb.geo.height, DOOMGENERIC_RESX, DOOMGENERIC_RESY,
           origin_x, origin_y, swap_rb ? " (swapping R/B)" : "");
}

void DG_DrawFrame(void) {
    if (!fb.geo.pitch) {
        return;
    }

    uint32_t rows = DOOMGENERIC_RESY;
    if (origin_y + rows > fb.geo.height) {
        rows = fb.geo.height - origin_y;
    }
    uint32_t cols = DOOMGENERIC_RESX;
    if (origin_x + cols > fb.geo.width) {
        cols = fb.geo.width - origin_x;
    }

    for (uint32_t y = 0; y < rows; y++) {
        /* Stride by pitch, never by width*4 — see struct
         * extron_fb_geometry. The framebuffer is mapped Normal-NC
         * precisely so this memcpy is legal; Device memory would fault
         * on its unaligned accesses. */
        void *dst = (void *)(fb.base + (uint64_t)(origin_y + y) * fb.geo.pitch
                                      + (uint64_t)origin_x * 4);
        const uint32_t *src = DG_ScreenBuffer + (size_t)y * DOOMGENERIC_RESX;

        if (swap_rb) {
            /* Fallback only: the firmware didn't give the byte order we
             * asked for, so red and blue must be exchanged per pixel.
             * Correct but much slower than the memcpy below, which is
             * why fb.c requests the matching order up front. */
            volatile uint32_t *d = (volatile uint32_t *)dst;
            for (uint32_t x = 0; x < cols; x++) {
                uint32_t px = src[x];
                d[x] = (px & 0xFF00FF00u) | ((px & 0x00FF0000u) >> 16)
                                          | ((px & 0x000000FFu) << 16);
            }
        } else {
            memcpy(dst, src, cols * 4);
        }
    }

    poll_input();
}

void DG_SleepMs(uint32_t ms) {
    __syscall2(SYS_SLEEP, ms / 1000, (long)(ms % 1000) * 1000000L);
}

uint32_t DG_GetTicksMs(void) {
    return (uint32_t)extron_uptime_ms();
}

int DG_GetKey(int *pressed, unsigned char *key) {
    if (key_read == key_write) {
        return 0;
    }
    uint16_t data = key_queue[key_read];
    key_read = (key_read + 1) % KEYQUEUE_SIZE;
    *pressed = data >> 8;
    *key     = data & 0xFF;
    return 1;
}

void DG_SetWindowTitle(const char *title) {
    (void)title;   /* no windows here */
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* The kernel now supplies a real SysV argc/argv stack for mlibc, but
     * there is still no shell to provide Doom's command line. Supply the
     * fixed arguments here. -iwad avoids unnecessary directory discovery
     * and names the WAD exposed through the ramfs directly. */
    static char arg0[] = "doom";
    static char arg1[] = "-iwad";
    static char arg2[] = "doom1.wad";
    static char *args[] = { arg0, arg1, arg2, NULL };

    doomgeneric_Create(3, args);
    for (;;) {
        doomgeneric_Tick();
    }
    return 0;
}
