#ifndef EXTRON_FB_H
#define EXTRON_FB_H

#include <stdint.h>

/*
 * The display, as handed to a process created with
 * PROC_MAP_FRAMEBUFFER (kernel/include/kernel/proc/exec.h).
 *
 * No syscall is involved: the kernel maps the framebuffer and this
 * descriptor into the address space at creation, the same way it maps
 * the stack and the ELF segments. A process either has a display from
 * the moment it starts or never does.
 *
 * Must match struct user_fb_info in kernel/proc/exec.c. Duplicated
 * rather than shared through a header for the same reason the syscall
 * numbers are: this is the ABI boundary, and userspace has no business
 * on the kernel's include path.
 */
#define EXTRON_FB_INFO_ADDR 0x50000000UL

struct extron_fb_info {
    uint64_t base;      /* VA of the framebuffer in THIS process */
    uint32_t width;
    uint32_t height;
    /* Bytes per row. NOT width * 4 in general — the firmware pads rows
     * to whatever alignment it likes, and computing a row address from
     * width instead is what shears the picture into diagonals. */
    uint32_t pitch;
    uint32_t depth;     /* bits per pixel */
    uint32_t rgb_order; /* 1 = RGB byte order, 0 = BGR */
    uint32_t size;      /* bytes */
};

static inline const struct extron_fb_info *extron_fb(void) {
    return (const struct extron_fb_info *)EXTRON_FB_INFO_ADDR;
}

/*
 * Raw input bytes from the UART, pushed by the kernel's ISR into a ring
 * mapped into this process. Polling it costs no syscall, which is the
 * point: SYS_READ blocks, and a game loop must ask "any key?" 35 times a
 * second without ever waiting.
 *
 * We own `tail`; the kernel owns `head`. Neither writes the other's
 * field, so no lock is needed.
 *
 * Must match struct kbd_ring in kernel/include/kernel/drivers/keyboard.h.
 */
#define EXTRON_INPUT_ADDR 0x4FFFF000UL
#define EXTRON_INPUT_SIZE 256

struct extron_input_ring {
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile unsigned char buf[EXTRON_INPUT_SIZE];
};

/* Next input byte, or -1 if none is waiting. Never blocks. */
static inline int extron_input_getc(void) {
    struct extron_input_ring *r = (struct extron_input_ring *)EXTRON_INPUT_ADDR;
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
