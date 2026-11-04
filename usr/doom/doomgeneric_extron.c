/*
 * doomgeneric platform layer for Extron (x86-64 and aarch64).
 *
 * The five DG_* functions doomgeneric asks a port to supply. The program is an
 * ordinary user process:
 *
 *   - the display is /dev/fb0: opened and mmap()'d here. Mapping it is what takes
 *     the screen from the kernel text console (kernel/drivers/fbown.c) and the
 *     keyboard from the tty; exiting, being killed or unmapping gives both back.
 *     Frames are plain stores to memory, with no syscall in the frame loop.
 *   - input comes from /dev/input, mapped the same way: key *events* with both
 *     edges when a real keyboard is attached (PS/2 today), or, over a serial
 *     console, a byte stream whose key releases have to be synthesised.
 *   - time is clock_gettime(CLOCK_MONOTONIC), sleeping is nanosleep().
 */
#include "doomkeys.h"
#include "doomgeneric.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include "../include/extron/fb.h"

#define DEFAULT_IWAD "/usr/share/games/doom/doom1.wad"

/* Geometry read once at DG_Init(); base is the mmap()'d pixel pointer. */
static struct {
    uint8_t *base;
    struct extron_fb_geometry geo;
} fb;

static uint32_t extron_uptime_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* Integer scale (DOOM's image is 640x400; a 1280x800 display gets exactly 2x) and
 * the top-left corner that centres the scaled image. */
static uint32_t scale = 1, origin_x, origin_y;
/* The display's byte order is the opposite of DOOM's 0xAARRGGBB: exchange red and
 * blue while copying. */
static int swap_rb;
static uint32_t *rowbuf;       /* one scaled output row */

/* ------------------------------------------------------------------------ */
/* Input                                                                     */
/* ------------------------------------------------------------------------ */

#define KEYQUEUE_SIZE 64
static uint16_t key_queue[KEYQUEUE_SIZE];
static unsigned key_write, key_read;

static void queue_key(int pressed, unsigned char key) {
    if (!key) {
        return;
    }
    unsigned next = (key_write + 1) % KEYQUEUE_SIZE;
    if (next == key_read) {
        return;                    /* full: drop */
    }
    key_queue[key_write] = (uint16_t)((pressed << 8) | key);
    key_write = next;
}

/* PS/2 set-1 make code (bit 7 = E0-prefixed) -> DOOM key. */
static unsigned char event_to_doom_key(unsigned code) {
    static const char row_q[]  = "qwertyuiop";      /* 0x10..0x19 */
    static const char row_a[]  = "asdfghjkl";       /* 0x1E..0x26 */
    static const char row_z[]  = "zxcvbnm";         /* 0x2C..0x32 */
    switch (code) {
        case 0x01: return KEY_ESCAPE;
        case 0x1C: case 0x9C: return KEY_ENTER;
        case 0x0E: return KEY_BACKSPACE;
        case 0x0F: return KEY_TAB;
        case 0x39: return KEY_USE;                   /* space */
        case 0x1D: case 0x9D: return KEY_FIRE;       /* Ctrl */
        case 0x2A: case 0x36: return KEY_RSHIFT;     /* run */
        case 0x38: case 0xB8: return KEY_RALT;       /* strafe */
        case 0xC8: return KEY_UPARROW;
        case 0xD0: return KEY_DOWNARROW;
        case 0xCB: return KEY_LEFTARROW;
        case 0xCD: return KEY_RIGHTARROW;
        case 0x33: return KEY_STRAFE_L;              /* , */
        case 0x34: return KEY_STRAFE_R;              /* . */
        case 0x0C: return KEY_MINUS;
        case 0x0D: return KEY_EQUALS;
        case 0x3A: return KEY_CAPSLOCK;
        case 0x1A: return '['; case 0x1B: return ']';
        case 0x27: return ';'; case 0x28: return '\'';
        case 0x29: return '`'; case 0x2B: return '\\'; case 0x35: return '/';
        case 0x57: return KEY_F11;
        case 0x58: return KEY_F12;
        default: break;
    }
    if (code >= 0x02 && code <= 0x0A) return (unsigned char)('1' + (code - 0x02));
    if (code == 0x0B) return '0';
    if (code >= 0x3B && code <= 0x44) return (unsigned char)(KEY_F1 + (code - 0x3B));
    if (code >= 0x10 && code <= 0x19) return (unsigned char)row_q[code - 0x10];
    if (code >= 0x1E && code <= 0x26) return (unsigned char)row_a[code - 0x1E];
    if (code >= 0x2C && code <= 0x32) return (unsigned char)row_z[code - 0x2C];
    return 0;
}

/*
 * Serial fallback. A terminal sends a byte when a key goes down and NOTHING when
 * it comes up, but DOOM needs both edges. Releases are synthesised: a key counts
 * as held until no further byte for it has arrived for a while. A terminal's
 * auto-repeat has a long initial delay then a fast stream, so the first byte arms
 * a long deadline and the first repeat switches to a short one.
 */
#define KEY_INITIAL_HOLD_MS 600
#define KEY_REPEAT_HOLD_MS  120
#define MAX_HELD            8

static struct {
    unsigned char key;
    uint32_t      last_ms;
    int           repeating;
} held[MAX_HELD];

/* Serial gives ASCII, not scancodes. Arrow keys arrive as escape sequences, so
 * WASD is offered alongside. */
static unsigned char byte_to_doom_key(int c) {
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
        case '1': case '2': case '3': case '4': case '5': case '6': case '7':
            return (unsigned char)c;
        case 127: case 8:     return KEY_BACKSPACE;
        default:              return 0;
    }
}

static void note_press(unsigned char key, uint32_t now) {
    for (int i = 0; i < MAX_HELD; i++) {
        if (held[i].key == key) {
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
        uint32_t limit = held[i].repeating ? KEY_REPEAT_HOLD_MS : KEY_INITIAL_HOLD_MS;
        if (held[i].key && (now - held[i].last_ms) > limit) {
            queue_key(0, held[i].key);
            held[i].key = 0;
        }
    }
}

static struct extron_input_ring *input_ring;
static int have_key_events;       /* a real keyboard has produced events */

static void poll_input(void) {
    if (!input_ring) return;
    int pressed;
    unsigned code;
    while (extron_input_getkey(input_ring, &pressed, &code)) {
        have_key_events = 1;
        queue_key(pressed, event_to_doom_key(code));
    }
    uint32_t now = extron_uptime_ms();
    int c;
    while ((c = extron_input_getc(input_ring)) >= 0) {
        if (have_key_events)
            continue;             /* the keyboard's bytes duplicate its events */
        unsigned char key = byte_to_doom_key(c);
        if (key) {
            note_press(key, now);
        }
    }
    if (!have_key_events)
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

/* ------------------------------------------------------------------------ */
/* Display                                                                   */
/* ------------------------------------------------------------------------ */

void DG_Init(void) {
    init_input_ring();

    int fd = open(EXTRON_FB_PATH, O_RDWR);
    if (fd < 0) {
        printf("DOOM: could not open %s\n", EXTRON_FB_PATH);
        return;
    }
    if (read(fd, &fb.geo, sizeof(fb.geo)) != (long)sizeof(fb.geo)
            || !fb.geo.width || !fb.geo.pitch) {
        printf("DOOM: no framebuffer\n");
        close(fd);
        return;
    }
    /* Mapping the display takes it from the text console; it is ours until this
     * process exits. */
    void *mapped = mmap(NULL, fb.geo.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);                        /* the mapping outlives the fd */
    if (mapped == MAP_FAILED) {
        printf("DOOM: cannot take over the display (is another program using it?)\n");
        fb.geo.pitch = 0;             /* sentinel DG_DrawFrame checks */
        return;
    }
    fb.base = mapped;

    uint32_t sx = fb.geo.width / DOOMGENERIC_RESX, sy = fb.geo.height / DOOMGENERIC_RESY;
    scale = sx < sy ? sx : sy;
    if (scale < 1) scale = 1;
    uint32_t w = DOOMGENERIC_RESX * scale, h = DOOMGENERIC_RESY * scale;
    origin_x = fb.geo.width  > w ? (fb.geo.width  - w) / 2 : 0;
    origin_y = fb.geo.height > h ? (fb.geo.height - h) / 2 : 0;
    swap_rb = (fb.geo.rgb_order != 0);
    rowbuf = malloc((size_t)DOOMGENERIC_RESX * scale * 4);

    /* Black the whole screen once, so the letterbox shows nothing of the console. */
    for (uint32_t y = 0; y < fb.geo.height; y++)
        memset(fb.base + (size_t)y * fb.geo.pitch, 0, (size_t)fb.geo.width * 4);

    printf("DOOM: %ux%u display, %ux%u image scaled %ux at +%u+%u%s\n",
           fb.geo.width, fb.geo.height, DOOMGENERIC_RESX, DOOMGENERIC_RESY, scale,
           origin_x, origin_y, swap_rb ? " (swapping R/B)" : "");
}

void DG_DrawFrame(void) {
    if (fb.geo.pitch && rowbuf) {
        uint32_t cols = DOOMGENERIC_RESX * scale;
        if (origin_x + cols > fb.geo.width) cols = fb.geo.width - origin_x;
        for (uint32_t y = 0; y < DOOMGENERIC_RESY; y++) {
            const uint32_t *src = DG_ScreenBuffer + (size_t)y * DOOMGENERIC_RESX;
            /* One output row: scale horizontally (and exchange R/B if needed). */
            uint32_t *out = rowbuf;
            for (uint32_t x = 0; x < DOOMGENERIC_RESX; x++) {
                uint32_t px = src[x];
                if (swap_rb)
                    px = (px & 0xFF00FF00u) | ((px & 0x00FF0000u) >> 16)
                                            | ((px & 0x000000FFu) << 16);
                for (uint32_t k = 0; k < scale; k++)
                    *out++ = px;
            }
            /* ...then store it `scale` times, striding by pitch (never width*4:
             * the firmware pads rows). */
            for (uint32_t k = 0; k < scale; k++) {
                uint32_t dy = origin_y + y * scale + k;
                if (dy >= fb.geo.height) break;
                memcpy(fb.base + (size_t)dy * fb.geo.pitch + (size_t)origin_x * 4,
                       rowbuf, (size_t)cols * 4);
            }
        }
    }
    poll_input();
}

void DG_SleepMs(uint32_t ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

uint32_t DG_GetTicksMs(void) {
    return extron_uptime_ms();
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
    /* doom [doomgeneric options]: -iwad defaults to the installed shareware WAD. */
    int have_iwad = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-iwad"))
            have_iwad = 1;

    char *args[64];
    int n = 0;
    for (int i = 0; i < argc && n < 60; i++)
        args[n++] = argv[i];
    if (!have_iwad) {
        args[n++] = (char *)"-iwad";
        args[n++] = (char *)DEFAULT_IWAD;
    }
    args[n] = NULL;

    doomgeneric_Create(n, args);
    for (;;) {
        doomgeneric_Tick();
    }
    return 0;
}
