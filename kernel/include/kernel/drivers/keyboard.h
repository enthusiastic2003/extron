#ifndef KERNEL_DRIVERS_KEYBOARD_H
#define KERNEL_DRIVERS_KEYBOARD_H

#include <stdint.h>

/*
 * There's no physical keyboard on this setup — a headless RPi4 talked
 * to over the same USB-TTL serial cable already used for console
 * output. "Keyboard input" here just means bytes arriving on the UART
 * RX line (GPIO15/RXD0), typed on whatever terminal is attached to the
 * other end. Genuinely tied to this exact board's PL011/GPIO wiring
 * (kernel/drivers/uart.c) — not a portable abstraction, no reason to
 * pretend otherwise.
 */
void init_kbd(void);
char kbd_getc(void); /* blocks until a byte arrives */
int kbd_getc_interruptible(char *out); /* 1=byte, -1=unblocked signal */
int kbd_try_getc(char *out); /* returns 1 when a byte was available, else 0 */
int kbd_input_ready(void);
int kbd_wait_for_input(int timeout_ms);
void kbd_flush_input(void);
void kbd_inject_byte(uint8_t c); /* feed one byte as if typed on the console */


/*
 * A single-producer/single-consumer ring of raw input bytes, in one page
 * that gets mapped into a process alongside the framebuffer.
 *
 * Exists because a game loop must poll input without blocking — DOOM
 * runs at 35 tics/second whether or not a key is waiting — while
 * kbd_getc()/SYS_READ block by design. Rather than add a non-blocking
 * read syscall, input is handed over the same way the display is: the
 * UART ISR pushes bytes in, the process drains them with plain loads,
 * and no svc happens in the frame loop at all.
 *
 * `head` is written only by the ISR and `tail` only by the process, so
 * the two never write the same field and no lock is needed between
 * them. The process can corrupt its own view by writing nonsense to
 * tail; the kernel reads tail only to decide whether the ring is full,
 * and treats it as untrusted (a bad value costs that process dropped
 * keystrokes, nothing more).
 */
#include <stdint.h>
#include <kernel/mm/pmm.h>

#define KBD_RING_SIZE 256

struct kbd_ring {
    volatile uint32_t head;   /* kernel writes */
    volatile uint32_t tail;   /* process writes */
    volatile uint8_t  buf[KBD_RING_SIZE];
    /* Key events (press AND release), for programs that need both edges. A
     * terminal-style byte stream cannot say when a key goes up; a keyboard
     * driver can. Each entry is (pressed << 8) | keycode, where keycode is the
     * PS/2 set-1 make code with bit 7 set for E0-prefixed ("extended") keys
     * (so Right Ctrl = 0x9D, Up = 0xC8). Same single-producer/single-consumer
     * rules as the byte ring. Serial input produces no events. */
    volatile uint32_t ev_head;   /* kernel writes */
    volatile uint32_t ev_tail;   /* process writes */
    volatile uint16_t ev[KBD_RING_SIZE];
};
_Static_assert(sizeof(struct kbd_ring) <= 4096, "the input ring must fit in one page");

/* One key press/release (see struct kbd_ring). */
void kbd_inject_event(uint8_t keycode, int pressed);

/* Physical address of the shared ring page, or 0 if it couldn't be
 * allocated. Set up by init_kbd(). */
phys_addr_t kbd_ring_phys(void);

#endif
