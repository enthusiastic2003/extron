#include <kernel/drivers/tty.h>
#include <kernel/drivers/serial.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/signal.h>
#include <arch/irq_spinlock.h>
#include <kernel/klibc/string.h>


#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSTART 8
#define VSTOP 9
#define VSUSP 10
#define IGNCR 0000200
#define ICRNL 0000400
#define INLCR 0000100
#define OPOST 0000001
#define ONLCR 0000004
#define CS8 0000060
#define CREAD 0000200
#define CLOCAL 0004000
#define ISIG   0000001
#define ICANON 0000002
#define ECHO   0000010
#define ECHOE  0000020
#define ECHOK  0000040

struct tty tty_table[MAX_TTYS];

#include <kernel/drivers/tty.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/serial.h>
#include <kernel/klibc/string.h>
#include <kernel/proc/signal.h>
#include <arch/irq_spinlock.h>
#include <stdbool.h>

/* Linux-compatible values used by Extron's mlibc ABI. */
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSTART 8
#define VSTOP 9
#define VSUSP 10

#define IGNCR 0000200
#define ICRNL 0000400
#define INLCR 0000100
#define OPOST 0000001
#define ONLCR 0000004
#define CS8 0000060
#define CREAD 0000200
#define CLOCAL 0004000
#define ICANON 0000002
#define ISIG   0000001
static void serial_write_out(struct tty *t, const char *buf, size_t count) {
    (void)t;
    for (size_t i = 0; i < count; i++) {
        char byte = buf[i];
        if ((t->termios.oflag & (OPOST | ONLCR)) == (OPOST | ONLCR) && byte == '\n')
            serial_putc('\r');
        serial_putc(byte);
    }
}

void tty_init(void) {
    for (int i = 0; i < MAX_TTYS; i++) {
        struct tty *t = &tty_table[i];
        t->lock = (spinlock_t)SPINLOCK_INIT;
        memset(&t->termios, 0, sizeof(t->termios));
        t->in_head = 0;
        t->in_tail = 0;
        t->canonical_length = 0;
        t->canonical_offset = 0;
        t->canonical_ready = false;
        t->canonical_eof = false;
        t->foreground_pgid = 1; /* Default PGID */
    }

    /* Initialize tty0 (console) */
    struct tty *console = &tty_table[0];
    console->termios.iflag = ICRNL;
    console->termios.oflag = OPOST | ONLCR;
    console->termios.cflag = CS8 | CREAD | CLOCAL;
    console->termios.lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK;
    console->termios.cc[VINTR] = 3;
    console->termios.cc[VQUIT] = 28;
    console->termios.cc[VERASE] = 127;
    console->termios.cc[VKILL] = 21;
    console->termios.cc[VEOF] = 4;
    console->termios.cc[VSTART] = 17;
    console->termios.cc[VSTOP] = 19;
    console->termios.cc[VSUSP] = 26;
    console->termios.ibaud = 115200;
    console->termios.obaud = 115200;
    console->winsize.row = 25;
    console->winsize.col = 80;
    console->write_out = serial_write_out;
}

static void emit_locked(struct tty *t, char byte) {
    if (t->write_out) {
        t->write_out(t, &byte, 1);
    }
}

void tty_push_input(struct tty *t, char c) {
    irq_spin_lock(&t->lock);
    uint32_t next = (t->in_head + 1) % TTY_BUFFER_SIZE;
    if (next != t->in_tail) {
        t->in_buf[t->in_head] = c;
        t->in_head = next;
    }
    irq_spin_unlock(&t->lock);
    wakeup(&t->in_buf);
}

static int tty_getc_interruptible(struct tty *t, char *out) {
    irq_spin_lock(&t->lock);
    while (t->in_head == t->in_tail) {
        sleep(&t->in_buf, &t->lock);
        if (signal_pending_unblocked(my_thread())) {
            irq_spin_unlock(&t->lock);
            return -1;
        }
    }
    *out = t->in_buf[t->in_tail];
    t->in_tail = (t->in_tail + 1) % TTY_BUFFER_SIZE;
    irq_spin_unlock(&t->lock);
    return 1;
}

static int tty_try_getc(struct tty *t, char *out) {
    irq_spin_lock(&t->lock);
    if (t->in_head == t->in_tail) {
        irq_spin_unlock(&t->lock);
        return 0;
    }
    *out = t->in_buf[t->in_tail];
    t->in_tail = (t->in_tail + 1) % TTY_BUFFER_SIZE;
    irq_spin_unlock(&t->lock);
    return 1;
}

bool tty_handle_input_byte(struct tty *t, uint8_t byte) {
    irq_spin_lock(&t->lock);
    if (!(t->termios.lflag & ISIG) || 
        (byte != t->termios.cc[VINTR] && byte != t->termios.cc[VQUIT] && byte != t->termios.cc[VSUSP])) {
        irq_spin_unlock(&t->lock);
        return false;
    }

    int signo = byte == t->termios.cc[VINTR] ? 2 : byte == t->termios.cc[VQUIT] ? 3 : 20;
    uint64_t pgid = t->foreground_pgid;
    t->canonical_length = 0;
    t->canonical_offset = 0;
    t->canonical_ready = false;
    t->canonical_eof = false;
    emit_locked(t, '\n');
    irq_spin_unlock(&t->lock);

    signal_send_group(pgid, signo);
    return true;
}

static char map_input_locked(struct tty *t, char byte, bool *discard) {
    *discard = false;
    if (byte == '\r') {
        if (t->termios.iflag & IGNCR) {
            *discard = true;
            return byte;
        }
        if (t->termios.iflag & ICRNL) return '\n';
    } else if (byte == '\n' && (t->termios.iflag & INLCR)) {
        return '\r';
    }
    return byte;
}

static long read_noncanonical(struct tty *t, char *buffer, size_t count) {
    irq_spin_lock(&t->lock);
    size_t minimum = t->termios.cc[VMIN];
    irq_spin_unlock(&t->lock);
    if (minimum > count) minimum = count;

    size_t received = 0;
    while (received < minimum) {
        char raw;
        if (tty_getc_interruptible(t, &raw) < 0)
            return received ? (long)received : -4; /* EINTR */
        irq_spin_lock(&t->lock);
        bool discard;
        char byte = map_input_locked(t, raw, &discard);
        if (!discard) {
            if (t->termios.lflag & ECHO) emit_locked(t, byte);
            buffer[received++] = byte;
        }
        irq_spin_unlock(&t->lock);
    }

    char raw;
    while (received < count && tty_try_getc(t, &raw)) {
        irq_spin_lock(&t->lock);
        bool discard;
        char byte = map_input_locked(t, raw, &discard);
        if (!discard) {
            if (t->termios.lflag & ECHO) emit_locked(t, byte);
            buffer[received++] = byte;
        }
        irq_spin_unlock(&t->lock);
    }
    return (long)received;
}

static int fill_canonical_line(struct tty *t) {
    for (;;) {
        char raw;
        if (tty_getc_interruptible(t, &raw) < 0) return -4;
        irq_spin_lock(&t->lock);
        bool discard;
        char byte = map_input_locked(t, raw, &discard);
        if (discard) { irq_spin_unlock(&t->lock); continue; }

        if ((uint8_t)byte == t->termios.cc[VERASE]) {
            if (t->canonical_length) {
                t->canonical_length--;
                if ((t->termios.lflag & (ECHO | ECHOE)) == (ECHO | ECHOE)) {
                    emit_locked(t, '\b'); emit_locked(t, ' '); emit_locked(t, '\b');
                } else if (t->termios.lflag & ECHO) emit_locked(t, byte);
            }
            irq_spin_unlock(&t->lock);
            continue;
        }

        if ((uint8_t)byte == t->termios.cc[VKILL]) {
            while (t->canonical_length) {
                t->canonical_length--;
                if ((t->termios.lflag & (ECHO | ECHOE)) == (ECHO | ECHOE)) {
                    emit_locked(t, '\b'); emit_locked(t, ' '); emit_locked(t, '\b');
                }
            }
            if ((t->termios.lflag & (ECHO | ECHOK)) == (ECHO | ECHOK)) emit_locked(t, '\n');
            irq_spin_unlock(&t->lock);
            continue;
        }

        if ((uint8_t)byte == t->termios.cc[VEOF]) {
            t->canonical_ready = true;
            t->canonical_eof = (t->canonical_length == 0);
            irq_spin_unlock(&t->lock);
            return 0;
        }

        if (t->canonical_length < TTY_CANON_MAX) t->canonical[t->canonical_length++] = byte;
        if (t->termios.lflag & ECHO) emit_locked(t, byte);
        if (byte == '\n' || t->canonical_length == TTY_CANON_MAX) {
            t->canonical_ready = true;
            irq_spin_unlock(&t->lock);
            return 0;
        }
        irq_spin_unlock(&t->lock);
    }
}

long tty_read(struct tty *t, void *buffer, size_t count) {
    if (!count) return 0;
    irq_spin_lock(&t->lock);
    bool canonical = t->termios.lflag & ICANON;
    irq_spin_unlock(&t->lock);
    if (!canonical) return read_noncanonical(t, buffer, count);

    irq_spin_lock(&t->lock);
    bool ready = t->canonical_ready;
    irq_spin_unlock(&t->lock);
    if (!ready) {
        if (fill_canonical_line(t) < 0) return -4;
    }

    irq_spin_lock(&t->lock);
    if (t->canonical_eof) {
        t->canonical_eof = false;
        t->canonical_ready = false;
        irq_spin_unlock(&t->lock);
        return 0;
    }
    size_t available = t->canonical_length - t->canonical_offset;
    if (count > available) count = available;
    memcpy(buffer, t->canonical + t->canonical_offset, count);
    t->canonical_offset += count;
    if (t->canonical_offset == t->canonical_length) {
        t->canonical_length = 0;
        t->canonical_offset = 0;
        t->canonical_ready = false;
    }
    irq_spin_unlock(&t->lock);
    return (long)count;
}

long tty_write(struct tty *t, const void *buffer, size_t count) {
    irq_spin_lock(&t->lock);
    if (t->write_out) t->write_out(t, buffer, count);
    irq_spin_unlock(&t->lock);
    return (long)count;
}

void tty_get_termios(struct tty *t, struct tty_termios *out) {
    irq_spin_lock(&t->lock); *out = t->termios; irq_spin_unlock(&t->lock);
}

void tty_set_termios(struct tty *t, const struct tty_termios *termios, int flush_input) {
    irq_spin_lock(&t->lock);
    t->termios = *termios;
    if (flush_input) {
        t->canonical_length = 0;
        t->canonical_offset = 0;
        t->canonical_ready = false;
        t->canonical_eof = false;
        t->in_tail = t->in_head;
    }
    irq_spin_unlock(&t->lock);
}

void tty_get_winsize(struct tty *t, struct tty_winsize *out) {
    irq_spin_lock(&t->lock); *out = t->winsize; irq_spin_unlock(&t->lock);
}

void tty_set_winsize(struct tty *t, const struct tty_winsize *ws) {
    irq_spin_lock(&t->lock); t->winsize = *ws; irq_spin_unlock(&t->lock);
}

uint64_t tty_foreground_pgid(struct tty *t) {
    irq_spin_lock(&t->lock); uint64_t pgid = t->foreground_pgid; irq_spin_unlock(&t->lock);
    return pgid;
}

void tty_set_foreground_pgid(struct tty *t, uint64_t pgid) {
    irq_spin_lock(&t->lock); t->foreground_pgid = pgid; irq_spin_unlock(&t->lock);
}

#include <kernel/drivers/timer.h>

void tty_flush_input(struct tty *t) {
    irq_spin_lock(&t->lock);
    t->in_tail = t->in_head;
    irq_spin_unlock(&t->lock);
}

int tty_input_ready(struct tty *t) {
    irq_spin_lock(&t->lock);
    int ready = t->in_head != t->in_tail;
    irq_spin_unlock(&t->lock);
    return ready;
}

int tty_wait_for_input(struct tty *t, int timeout_ms) {
    irq_spin_lock(&t->lock);
    if (t->in_head == t->in_tail && timeout_ms != 0) {
        struct thread *th = my_thread();
        if (timeout_ms > 0) {
            uint64_t ticks = ((uint64_t)timeout_ms * timer_ticks_per_second() + 999) / 1000;
            if (!ticks) ticks = 1;
            th->sleep_until = timer_ticks() + ticks;
        } else {
            th->sleep_until = 0;
        }
        sleep(&t->in_buf, &t->lock);
        th->sleep_until = 0;
    }
    int ready = t->in_head != t->in_tail;
    irq_spin_unlock(&t->lock);
    return ready;
}
