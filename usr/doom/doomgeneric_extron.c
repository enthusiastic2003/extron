/*
 * doomgeneric platform layer for Extron.
 *
 * The five DG_* functions doomgeneric asks a port to supply, and
 * nothing else — no Extron-specific syscalls were added for this. Both
 * of the resources DOOM needs beyond ordinary libc are handed to the
 * process at creation rather than fetched at runtime:
 *
 *   - the framebuffer, mapped by exec (PROC_MAP_FRAMEBUFFER), so
 *     DG_DrawFrame is plain stores to memory with no svc in the frame
 *     loop
 *   - a keystroke ring, mapped the same way, so DG_GetKey can poll
 *     without blocking — which SYS_READ cannot do
 */
#include "doomkeys.h"
#include "doomgeneric.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <extron/syscall.h>
#include <extron/fb.h>

static const struct extron_fb_info *fb;

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
#define KEY_HOLD_MS 220
#define MAX_HELD    8

static struct {
    unsigned char key;
    uint32_t      last_ms;
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
            held[i].last_ms = now;   /* auto-repeat: still down */
            return;
        }
    }
    for (int i = 0; i < MAX_HELD; i++) {
        if (!held[i].key) {
            held[i].key = key;
            held[i].last_ms = now;
            queue_key(1, key);
            return;
        }
    }
}

static void expire_holds(uint32_t now) {
    for (int i = 0; i < MAX_HELD; i++) {
        if (held[i].key && (now - held[i].last_ms) > KEY_HOLD_MS) {
            queue_key(0, held[i].key);
            held[i].key = 0;
        }
    }
}

static void poll_input(void) {
    uint32_t now = (uint32_t)sys_uptime_ms();
    int c;
    while ((c = extron_input_getc()) >= 0) {
        unsigned char key = to_doom_key(c);
        if (key) {
            note_press(key, now);
        }
    }
    expire_holds(now);
}

void DG_Init(void) {
    fb = extron_fb();

    if (!fb->width || !fb->pitch) {
        printf("DOOM: no framebuffer mapped\n");
        return;
    }

    /* Centre DOOM's image on whatever the display turned out to be. It
     * is 640x400 against a 640x480 panel here, but neither number is
     * assumed — a smaller display just gets clipped by DG_DrawFrame. */
    origin_x = fb->width  > DOOMGENERIC_RESX ? (fb->width  - DOOMGENERIC_RESX) / 2 : 0;
    origin_y = fb->height > DOOMGENERIC_RESY ? (fb->height - DOOMGENERIC_RESY) / 2 : 0;

    /* rgb_order == 1 means byte 0 is red, i.e. 0xAABBGGRR — the opposite
     * of what DOOM writes. */
    swap_rb = (fb->rgb_order != 0);

    /* Black the whole panel once, so the letterbox around the image
     * isn't left showing whatever was on screen before. */
    for (uint32_t y = 0; y < fb->height; y++) {
        volatile uint32_t *row =
            (volatile uint32_t *)(fb->base + (uint64_t)y * fb->pitch);
        for (uint32_t x = 0; x < fb->width; x++) {
            row[x] = 0xFF000000u;
        }
    }

    printf("DOOM: %ux%u display, %ux%u image at +%u+%u%s\n",
           fb->width, fb->height, DOOMGENERIC_RESX, DOOMGENERIC_RESY,
           origin_x, origin_y, swap_rb ? " (swapping R/B)" : "");
}

void DG_DrawFrame(void) {
    if (!fb || !fb->pitch) {
        return;
    }

    uint32_t rows = DOOMGENERIC_RESY;
    if (origin_y + rows > fb->height) {
        rows = fb->height - origin_y;
    }
    uint32_t cols = DOOMGENERIC_RESX;
    if (origin_x + cols > fb->width) {
        cols = fb->width - origin_x;
    }

    for (uint32_t y = 0; y < rows; y++) {
        /* Stride by pitch, never by width*4 — see struct extron_fb_info.
         * The framebuffer is mapped Normal-NC precisely so this memcpy
         * is legal; Device memory would fault on its unaligned accesses. */
        void *dst = (void *)(fb->base + (uint64_t)(origin_y + y) * fb->pitch
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
    sys_sleep(ms / 1000, (long)(ms % 1000) * 1000000L);
}

uint32_t DG_GetTicksMs(void) {
    return (uint32_t)sys_uptime_ms();
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

    /* There is no command line to inherit — crt0 hands main() argc=0 —
     * so the arguments DOOM needs are supplied here. -iwad names the WAD
     * directly rather than letting d_iwad.c hunt through directories
     * that do not exist on this system; the name is resolved by fopen(),
     * which our libc backs with SYS_MAP_INITRD. */
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
