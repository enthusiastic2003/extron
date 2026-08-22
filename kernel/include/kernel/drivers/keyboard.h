#ifndef KERNEL_DRIVERS_KEYBOARD_H
#define KERNEL_DRIVERS_KEYBOARD_H

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

#endif
