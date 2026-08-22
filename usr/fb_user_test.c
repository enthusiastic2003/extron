/*
 * Draws to the display from EL0, with no syscall in the drawing path —
 * the framebuffer was mapped into this process at creation
 * (PROC_MAP_FRAMEBUFFER), so these are plain stores to memory.
 *
 * Deliberately draws HORIZONTAL bars where the kernel's own test pattern
 * (kernel/drivers/fb.c) draws vertical ones. If the picture changes
 * orientation, userspace unambiguously reached the display; a shared
 * pattern could leave you looking at the kernel's output and calling it
 * a success.
 */
#include <stdio.h>
#include <string.h>
#include <extron/syscall.h>
#include <extron/fb.h>

int main(void) {
    const struct extron_fb_info *fb = extron_fb();

    printf("FBUSER: %ux%u pitch=%u depth=%u %s base=0x%lx\n",
           fb->width, fb->height, fb->pitch, fb->depth,
           fb->rgb_order ? "RGB" : "BGR", (unsigned long)fb->base);

    if (!fb->width || !fb->height || !fb->pitch) {
        printf("FBUSER: no framebuffer mapped\n");
        return 1;
    }

    volatile unsigned char *base = (volatile unsigned char *)fb->base;

    /* Byte order is a runtime answer, so build pixels from it rather
     * than baking in a constant — same reason fb_rgb() exists kernel
     * side. Little-endian: with RGB order, red is the low byte. */
    #define PIX(r, g, b) (fb->rgb_order \
        ? (0xFF000000u | ((unsigned)(b) << 16) | ((unsigned)(g) << 8) | (unsigned)(r)) \
        : (0xFF000000u | ((unsigned)(r) << 16) | ((unsigned)(g) << 8) | (unsigned)(b)))

    static const struct { unsigned char r, g, b; } bars[] = {
        {255,   0,   0}, {255, 128,   0}, {255, 255,   0}, {  0, 255,   0},
        {  0, 255, 255}, {  0,   0, 255}, {255,   0, 255}, {255, 255, 255},
    };
    const unsigned n = sizeof(bars) / sizeof(bars[0]);
    unsigned bar_h = fb->height / n;

    for (unsigned y = 0; y < fb->height; y++) {
        unsigned idx = y / bar_h;
        if (idx >= n) idx = n - 1;
        unsigned colour = PIX(bars[idx].r, bars[idx].g, bars[idx].b);
        /* Row stride is pitch, never width*4. */
        volatile unsigned *row = (volatile unsigned *)(base + (unsigned long)y * fb->pitch);
        for (unsigned x = 0; x < fb->width; x++) {
            row[x] = colour;
        }
    }

    /* A black square dead centre — its position depends on width AND
     * height being right, so it catches a geometry error that uniform
     * bars would hide. */
    unsigned cx = fb->width / 2, cy = fb->height / 2, s = 48;
    for (unsigned y = cy - s; y < cy + s; y++) {
        volatile unsigned *row = (volatile unsigned *)(base + (unsigned long)y * fb->pitch);
        for (unsigned x = cx - s; x < cx + s; x++) {
            row[x] = PIX(0, 0, 0);
        }
    }

    printf("FBUSER: drew %u horizontal bars from EL0\n", n);
    return 0;
}
