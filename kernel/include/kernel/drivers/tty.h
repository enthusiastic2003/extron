#ifndef KERNEL_DRIVERS_TTY_H
#define KERNEL_DRIVERS_TTY_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Extron's userspace termios ABI. Keep this layout synchronized with
 * sysdeps/extron/include/abi-bits/termios.h. */
#define TTY_NCCS 32

struct tty_termios {
    uint32_t iflag;
    uint32_t oflag;
    uint32_t cflag;
    uint32_t lflag;
    uint8_t line;
    uint8_t cc[TTY_NCCS];
    uint32_t ibaud;
    uint32_t obaud;
};

struct tty_winsize {
    uint16_t row;
    uint16_t col;
    uint16_t xpixel;
    uint16_t ypixel;
};

void tty_init(void);
bool tty_handle_input_byte(uint8_t byte);
long tty_read(void *buffer, size_t count);
long tty_write(const void *buffer, size_t count);
void tty_get_termios(struct tty_termios *out);
void tty_set_termios(const struct tty_termios *termios, int flush_input);
void tty_get_winsize(struct tty_winsize *out);
void tty_set_winsize(const struct tty_winsize *ws);
uint64_t tty_foreground_pgid(void);
void tty_set_foreground_pgid(uint64_t pgid);

#endif
