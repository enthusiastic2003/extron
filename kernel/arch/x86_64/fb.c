#include <kernel/drivers/fb.h>
#include <kernel/mm/paging.h>
#include <kernel/console.h>
#include <boot/multiboot2.h>
#include <kernel/klibc/string.h>

/*
 * Framebuffer from the UEFI Graphics Output Protocol. The loader leaves the
 * firmware's current mode alone and hands over a multiboot2 framebuffer tag
 * (address, pitch, size, channel positions); this driver maps that memory
 * into the kernel's high half and implements the same fb.h API the Raspberry
 * Pi mailbox driver provides, so devfs's /dev/fb0 and fbcon work unchanged.
 */
static struct framebuffer fb;
static int fb_ready;
static uint32_t red_pos, green_pos, blue_pos;
static uint64_t boot_info;

void fb_set_boot_info(uint64_t mb2_virt) { boot_info = mb2_virt; }

const struct framebuffer *fb_init(uint32_t want_width, uint32_t want_height) {
    (void)want_width; (void)want_height;
    if (fb_ready) return &fb;
    if (!boot_info) return NULL;

    struct multiboot_tag *tag = (struct multiboot_tag *)(boot_info + 8);
    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_FRAMEBUFFER) {
            struct multiboot_tag_framebuffer *t = (struct multiboot_tag_framebuffer *)tag;
            if (t->common.framebuffer_type != MULTIBOOT_FRAMEBUFFER_TYPE_RGB ||
                t->common.framebuffer_bpp != 32)
                return NULL;
            fb.phys   = t->common.framebuffer_addr;
            fb.width  = t->common.framebuffer_width;
            fb.height = t->common.framebuffer_height;
            fb.pitch  = t->common.framebuffer_pitch;
            fb.depth  = 32;
            red_pos   = t->framebuffer_red_field_position;
            green_pos = t->framebuffer_green_field_position;
            blue_pos  = t->framebuffer_blue_field_position;
            fb.rgb_order = red_pos == 0;
            fb.size   = fb.pitch * fb.height;

            uint64_t start = fb.phys & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t end = (fb.phys + fb.size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
            for (uint64_t p = start; p < end; p += PAGE_SIZE) {
                if (kmap(NEW_HDDM + p, p, PAGE_PRESENT | PAGE_WRITE | PAGE_NX | PAGE_NORMAL_NC) != 0) {
                    kprintf("fb: mapping failed at %p\n", (void *)p);
                    return NULL;
                }
            }
            fb.base = NEW_HDDM + fb.phys;
            fb_ready = 1;
            kprintf("fb: %ux%u, pitch %u, %s at phys 0x%lx\n", fb.width, fb.height, fb.pitch,
                    fb.rgb_order ? "RGB" : "BGR", (unsigned long)fb.phys);
            return &fb;
        }
        tag = (struct multiboot_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7));
    }
    return NULL;
}

const struct framebuffer *fb_get(void) { return fb_ready ? &fb : NULL; }

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << red_pos) | ((uint32_t)g << green_pos) | ((uint32_t)b << blue_pos);
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t colour) {
    if (!fb_ready || x >= fb.width || y >= fb.height) return;
    if (w > fb.width - x) w = fb.width - x;
    if (h > fb.height - y) h = fb.height - y;
    for (uint32_t row = 0; row < h; row++) {
        uint32_t *p = (uint32_t *)(fb.base + (uint64_t)(y + row) * fb.pitch) + x;
        for (uint32_t i = 0; i < w; i++) p[i] = colour;
    }
}

void fb_fill(uint32_t colour) { fb_fill_rect(0, 0, fb.width, fb.height, colour); }

void fb_test_pattern(void) {
    if (!fb_ready) return;
    static const uint8_t bars[8][3] = {{255,255,255},{255,255,0},{0,255,255},{0,255,0},
                                       {255,0,255},{255,0,0},{0,0,255},{0,0,0}};
    uint32_t bw = fb.width / 8;
    for (int i = 0; i < 8; i++)
        fb_fill_rect(i * bw, 0, bw, fb.height, fb_rgb(bars[i][0], bars[i][1], bars[i][2]));
}
