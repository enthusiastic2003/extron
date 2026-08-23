#ifndef EXTRON_FB_H
#define EXTRON_FB_H

#include <stdint.h>

/*
 * The display, as a real device file: open(EXTRON_FB_PATH), read() the
 * geometry into a struct extron_fb_geometry, mmap() the fd for the
 * pixels themselves (kernel/fs/devfs.c's fb0 node, dispatched through
 * the real mmap()/munmap() syscalls — kernel/proc/syscall.c's
 * sys_mmap()). No process-creation-time special case: any process that
 * can open the device can draw to it, the same way any other file works.
 *
 * struct extron_fb_geometry must match struct fb_geometry in
 * kernel/fs/devfs.c byte for byte. Duplicated rather than shared
 * through a header for the same reason the syscall numbers are: this is
 * the ABI boundary, and userspace has no business on the kernel's
 * include path.
 */
#define EXTRON_FB_PATH "/dev/fb0"

struct extron_fb_geometry {
    uint32_t width;
    uint32_t height;
    /* Bytes per row. NOT width * 4 in general — the firmware pads rows
     * to whatever alignment it likes, and computing a row address from
     * width instead is what shears the picture into diagonals. */
    uint32_t pitch;
    uint32_t depth;     /* bits per pixel */
    uint32_t rgb_order; /* 1 = RGB byte order, 0 = BGR */
    uint32_t size;      /* bytes — the mmap() length that maps the whole thing */
};

/*
 * Raw input bytes from the UART, pushed by the kernel's ISR into a ring
 * — a real device, /dev/input (kernel/fs/devfs.c), opened and mmap()'d
 * like any other file. Polling costs no syscall once mapped, which is
 * the point: SYS_READ blocks, and a game loop must ask "any key?" 35
 * times a second without ever waiting.
 *
 * We own `tail`; the kernel owns `head`. Neither writes the other's
 * field, so no lock is needed. Cacheable (unlike the framebuffer): this
 * is ordinary RAM shared with the kernel's own ISR, not memory an
 * outside bus master scans continuously, so the dmb ish barriers below
 * are what order it, the same as any two cache-coherent contexts on one
 * machine.
 *
 * Must match struct kbd_ring in kernel/include/kernel/drivers/keyboard.h.
 */
#define EXTRON_INPUT_PATH "/dev/input"
#define EXTRON_INPUT_SIZE 256

struct extron_input_ring {
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile unsigned char buf[EXTRON_INPUT_SIZE];
};

/* Next input byte from an mmap()'d ring, or -1 if none is waiting.
 * Never blocks. `r` is the pointer mmap(EXTRON_INPUT_PATH's fd) returned. */
static inline int extron_input_getc(struct extron_input_ring *r) {
    uint32_t tail = r->tail % EXTRON_INPUT_SIZE;
    if (r->head == tail) {
        return -1;
    }
    unsigned char c = r->buf[tail];
    /* Consume the byte before advertising the slot as free, so the ISR
     * cannot overwrite it in the window between the two. */
    __asm__ volatile ("dmb ish" ::: "memory");
    r->tail = (tail + 1) % EXTRON_INPUT_SIZE;
    return (int)c;
}

#endif
