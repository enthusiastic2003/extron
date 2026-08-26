#ifndef KERNEL_DRIVERS_TTY_H
#define KERNEL_DRIVERS_TTY_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Extron's userspace termios ABI. Keep this layout synchronized with
 * sysdeps/extron/include/abi-bits/termios.h. */
#include <arch/irq_spinlock.h>

#define TTY_NCCS 32
#define TTY_CANON_MAX 4096
#define TTY_BUFFER_SIZE 1024

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

struct tty {
    spinlock_t lock;

    /* Configuration */
    struct tty_termios termios;
    struct tty_winsize winsize;
    uint64_t foreground_pgid;

    /* Raw input ring buffer (from hardware or PTY master) */
    char     in_buf[TTY_BUFFER_SIZE];
    uint32_t in_head;
    uint32_t in_tail;

    /* Callbacks */
    long (*write_out)(struct tty *t, const char *buf, size_t count,
                      bool nonblock);
    void *private_data;

    /* Canonical line editor state */
    char canonical[TTY_CANON_MAX];
    size_t canonical_length;
    size_t canonical_offset;
    bool canonical_ready;
    bool canonical_eof;
};

#define MAX_TTYS 16
extern struct tty tty_table[MAX_TTYS];

void tty_init(void);
void tty_push_input(struct tty *t, char c);
long tty_push_input_flags(struct tty *t, char c, bool nonblock);

bool tty_handle_input_byte(struct tty *t, uint8_t byte);
long tty_read(struct tty *t, void *buffer, size_t count);
long tty_write(struct tty *t, const void *buffer, size_t count);
long tty_read_flags(struct tty *t, void *buffer, size_t count, bool nonblock);
long tty_write_flags(struct tty *t, const void *buffer, size_t count, bool nonblock);
void tty_get_termios(struct tty *t, struct tty_termios *out);
void tty_set_termios(struct tty *t, const struct tty_termios *termios, int flush_input);
void tty_get_winsize(struct tty *t, struct tty_winsize *out);
void tty_set_winsize(struct tty *t, const struct tty_winsize *ws);
uint64_t tty_foreground_pgid(struct tty *t);
void tty_set_foreground_pgid(struct tty *t, uint64_t pgid);


void tty_flush_input(struct tty *t);
int tty_input_ready(struct tty *t);
int tty_input_space(struct tty *t);
int tty_output_ready(struct tty *t);
int tty_wait_for_input(struct tty *t, int timeout_ms);
#endif
