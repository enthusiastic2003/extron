#include <kernel/drivers/fb.h>
#include <kernel/drivers/mailbox.h>
#include <kernel/mm/paging.h>
#include <kernel/console.h>

static struct framebuffer fb;
static int fb_ready;

/* One message, seven tags. The firmware walks them in order, so the
 * "set" tags land before FB_ALLOCATE acts on them, and every tag's
 * answer is written back over its own request slots. 16-byte aligned
 * and in .bss for the same reasons as mailbox.c's own buffer. */
static volatile uint32_t fb_msg[36] __attribute__((aligned(16)));

const struct framebuffer *fb_get(void) {
    return fb_ready ? &fb : NULL;
}

const struct framebuffer *fb_init(uint32_t want_width, uint32_t want_height) {
    int i = 0;
    fb_msg[i++] = 0;                            /* total size, patched below */
    fb_msg[i++] = 0;                            /* request */

    fb_msg[i++] = TAG_FB_SET_PHYS_SIZE;
    fb_msg[i++] = 8; fb_msg[i++] = 8;
    int phys_size = i;
    fb_msg[i++] = want_width; fb_msg[i++] = want_height;

    fb_msg[i++] = TAG_FB_SET_VIRT_SIZE;
    fb_msg[i++] = 8; fb_msg[i++] = 8;
    fb_msg[i++] = want_width; fb_msg[i++] = want_height;

    /* Virtual size can exceed physical size to allow scrolling or page
     * flipping by moving this offset. We want neither yet, so pin it at
     * the origin rather than inheriting whatever was left set. */
    fb_msg[i++] = TAG_FB_SET_VIRT_OFFSET;
    fb_msg[i++] = 8; fb_msg[i++] = 8;
    fb_msg[i++] = 0; fb_msg[i++] = 0;

    fb_msg[i++] = TAG_FB_SET_DEPTH;
    fb_msg[i++] = 4; fb_msg[i++] = 4;
    int depth = i;
    fb_msg[i++] = 32;

    /* Ask for BGR *byte* order, which on a little-endian machine is what
     * puts a pixel in memory as 0xAARRGGBB — red in bits 16-23. That is
     * the layout essentially all software composes into, DOOM included
     * (its I_InitGraphics reports red_off:16). Requesting "RGB" here
     * gives byte 0 = red, i.e. 0xAABBGGRR, and every image comes out
     * with red and blue swapped — which looks like a broken display
     * rather than a naming convention. The value read back is what
     * fb_rgb() actually uses either way. */
    fb_msg[i++] = TAG_FB_SET_PIXEL_ORDER;
    fb_msg[i++] = 4; fb_msg[i++] = 4;
    int order = i;
    fb_msg[i++] = 0;                            /* 0 = BGR bytes = 0xAARRGGBB */

    /* Request 4096-byte alignment so the base lands on a page boundary —
     * it has to be mapped, and an unaligned base would mean mapping a
     * page the framebuffer only partly occupies. */
    fb_msg[i++] = TAG_FB_ALLOCATE;
    fb_msg[i++] = 8; fb_msg[i++] = 4;
    int alloc = i;
    fb_msg[i++] = PAGE_SIZE; fb_msg[i++] = 0;

    fb_msg[i++] = TAG_FB_GET_PITCH;
    fb_msg[i++] = 4; fb_msg[i++] = 0;
    int pitch = i;
    fb_msg[i++] = 0;

    fb_msg[i++] = 0;                            /* end tag */
    fb_msg[0] = (uint32_t)(i * 4);

    if (mailbox_property_call(fb_msg) != 0) {
        kprintf("fb: framebuffer request FAILED\n");
        return NULL;
    }

    uint32_t bus_base = fb_msg[alloc];
    uint32_t size     = fb_msg[alloc + 1];
    if (!bus_base || !size) {
        kprintf("fb: firmware allocated nothing (base 0x%x size 0x%x)\n",
                bus_base, size);
        return NULL;
    }

    /* Every value below is the firmware's answer, not our request. They
     * genuinely differ: this display negotiates 4:3, so asking for a
     * 16:9 mode gets something else back. */
    fb.phys      = (phys_addr_t)BUS_TO_PHYS(bus_base);
    fb.size      = size;
    fb.width     = fb_msg[phys_size];
    fb.height    = fb_msg[phys_size + 1];
    fb.depth     = fb_msg[depth];
    fb.rgb_order = fb_msg[order];
    fb.pitch     = fb_msg[pitch];

    if (fb.depth != 32) {
        kprintf("fb: firmware gave %u bpp, only 32 is handled\n", fb.depth);
        return NULL;
    }
    if (fb.pitch == 0) {
        kprintf("fb: firmware reported a zero pitch\n");
        return NULL;
    }

    /*
     * Map it into the kernel's own table. This is NOT optional and not
     * covered by the HHDM: init_paging() maps only regions the memory
     * map calls AVAILABLE, and the framebuffer lives in the VideoCore's
     * memory — the hole between the two available regions, which
     * GET_VC_MEMORY confirms independently. phys_to_virt_hhdm() on this
     * address would hand back a pointer to nothing.
     *
     * PAGE_NORMAL_NC, not PAGE_CACHE_DISABLE: the GPU scans this memory
     * out continuously, so it must not sit dirty in our caches, but
     * Device memory would forbid unaligned access outright — and the
     * whole point of a framebuffer is that something memcpy()s frames
     * into it.
     */
    uint32_t span = fb.size;
    for (uint32_t off = 0; off < span; off += PAGE_SIZE) {
        if (kmap(NEW_HDDM + fb.phys + off, fb.phys + off,
                 PAGE_PRESENT | PAGE_WRITE | PAGE_NX | PAGE_NORMAL_NC) != 0) {
            kprintf("fb: failed to map framebuffer at 0x%lx\n",
                    (unsigned long)(fb.phys + off));
            return NULL;
        }
    }
    fb.base = (virt_addr_t)(NEW_HDDM + fb.phys);
    flush_tlb();

    fb_ready = 1;
    kprintf("fb: %ux%u %ubpp pitch=%u %s phys=0x%lx size=%u KiB\n",
            fb.width, fb.height, fb.depth, fb.pitch,
            fb.rgb_order ? "RGB" : "BGR",
            (unsigned long)fb.phys, fb.size >> 10);
    if (fb.pitch != fb.width * 4) {
        /* Worth saying out loud: this is exactly the assumption that
         * produces a diagonally skewed picture when it is made wrong. */
        kprintf("fb: note pitch %u != width*4 (%u) — rows are padded\n",
                fb.pitch, fb.width * 4);
    }
    return &fb;
}

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) {
    /* rgb_order describes BYTE order in memory. Little-endian, so with
     * RGB order byte 0 (red) is the low byte of the word. */
    if (fb.rgb_order) {
        return (uint32_t)0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
    }
    return (uint32_t)0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t colour) {
    if (!fb_ready) return;
    if (x >= fb.width || y >= fb.height) return;
    if (x + w > fb.width)  w = fb.width - x;
    if (y + h > fb.height) h = fb.height - y;

    for (uint32_t row = 0; row < h; row++) {
        /* Row stride comes from pitch, never from width — see fb.h. */
        volatile uint32_t *p =
            (volatile uint32_t *)(fb.base + (uint64_t)(y + row) * fb.pitch + (uint64_t)x * 4);
        for (uint32_t col = 0; col < w; col++) {
            p[col] = colour;
        }
    }
}

void fb_fill(uint32_t colour) {
    fb_fill_rect(0, 0, fb.width, fb.height, colour);
}

void fb_test_pattern(void) {
    if (!fb_ready) return;

    static const struct { uint8_t r, g, b; } bars[] = {
        {255,   0,   0}, {  0, 255,   0}, {  0,   0, 255},
        {255, 255,   0}, {  0, 255, 255}, {255,   0, 255},
        {255, 255, 255}, { 32,  32,  32},
    };
    const uint32_t n = sizeof(bars) / sizeof(bars[0]);

    fb_fill(fb_rgb(0, 0, 0));

    /* Vertical bars: if the pitch is wrong, each row starts at the wrong
     * offset and the bars shear into diagonals — unmistakable, and it
     * says "pitch" specifically rather than just "broken". */
    uint32_t bar_w = fb.width / n;
    for (uint32_t i = 0; i < n; i++) {
        fb_fill_rect(i * bar_w, 0, bar_w, fb.height,
                     fb_rgb(bars[i].r, bars[i].g, bars[i].b));
    }

    /* Corner markers, inset. If width/height are wrong these land off
     * the visible area or float in from the edge, which distinguishes a
     * geometry error from a pitch error. */
    const uint32_t m = 32;
    uint32_t white = fb_rgb(255, 255, 255);
    fb_fill_rect(0, 0, m, m, white);
    fb_fill_rect(fb.width - m, 0, m, m, white);
    fb_fill_rect(0, fb.height - m, m, m, white);
    fb_fill_rect(fb.width - m, fb.height - m, m, m, white);

    /* A one-pixel border traces the exact reported geometry. */
    fb_fill_rect(0, 0, fb.width, 1, white);
    fb_fill_rect(0, fb.height - 1, fb.width, 1, white);
    fb_fill_rect(0, 0, 1, fb.height, white);
    fb_fill_rect(fb.width - 1, 0, 1, fb.height, white);

    kprintf("fb: test pattern drawn (%u bars)\n", n);
}
