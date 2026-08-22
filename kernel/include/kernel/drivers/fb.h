#ifndef KERNEL_DRIVERS_FB_H
#define KERNEL_DRIVERS_FB_H

#include <stdint.h>
#include <stddef.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>

/*
 * Linear framebuffer, allocated from the VideoCore via the mailbox
 * property channel (kernel/drivers/mailbox.h).
 *
 * The firmware is the authority on every field here. We ask for a size
 * and a depth; what comes back can differ, and using the requested
 * values instead of the returned ones is the classic way to get a
 * picture that is skewed, half off-screen, or colour-swapped. In
 * particular `pitch` is NOT width * bytes_per_pixel — the firmware pads
 * rows to whatever alignment it likes, so every row address must be
 * computed with pitch.
 */
struct framebuffer {
    virt_addr_t base;        /* kernel VA — see fb_init()'s mapping note */
    phys_addr_t phys;        /* ARM physical, already de-bussed */
    uint32_t    size;        /* bytes the firmware allocated */
    uint32_t    width;       /* actual, as reported back */
    uint32_t    height;      /* actual, as reported back */
    uint32_t    pitch;       /* bytes per row — NOT width * bpp */
    uint32_t    depth;       /* bits per pixel, as reported back */
    uint32_t    rgb_order;   /* 1 = RGB byte order, 0 = BGR */
};

/*
 * Ask the firmware for a framebuffer of the given size at 32bpp and map
 * it. Returns NULL on failure. The returned struct is owned by the
 * driver; it stays valid for the life of the system.
 */
const struct framebuffer *fb_init(uint32_t want_width, uint32_t want_height);

/* NULL until fb_init() succeeds. */
const struct framebuffer *fb_get(void);

/* Pack a colour for this framebuffer's reported byte order. Not a
 * constant expression on purpose: which of RGB/BGR the firmware
 * actually gave us is a runtime answer. */
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);

void fb_fill(uint32_t colour);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t colour);

/*
 * Draw a pattern chosen so that the usual framebuffer mistakes are
 * visually distinct rather than all looking like "wrong picture":
 * vertical colour bars skew diagonally if the pitch is wrong, the
 * corner markers land in the wrong places if width/height are wrong,
 * and the bar colours come out permuted if the byte order is wrong.
 */
void fb_test_pattern(void);

#endif
