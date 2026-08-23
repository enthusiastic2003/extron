#include <kernel/drivers/tty.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/serial.h>
#include <kernel/klibc/string.h>
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
#define ECHO 0000010
#define ECHOE 0000020
#define ECHOK 0000040

#define TTY_CANON_MAX 1024

struct console_tty {
    spinlock_t lock;
    struct tty_termios termios;
    struct tty_winsize winsize;
    char canonical[TTY_CANON_MAX];
    size_t canonical_length;
    size_t canonical_offset;
    bool canonical_ready;
    bool canonical_eof;
};

static struct console_tty console_tty = { .lock = SPINLOCK_INIT };

static void emit_locked(char byte) {
    if ((console_tty.termios.oflag & (OPOST | ONLCR)) == (OPOST | ONLCR)
            && byte == '\n')
        serial_putc('\r');
    serial_putc(byte);
}

static char map_input_locked(char byte, bool *discard) {
    *discard = false;
    if (byte == '\r') {
        if (console_tty.termios.iflag & IGNCR) {
            *discard = true;
            return byte;
        }
        if (console_tty.termios.iflag & ICRNL)
            return '\n';
    } else if (byte == '\n' && (console_tty.termios.iflag & INLCR)) {
        return '\r';
    }
    return byte;
}

static void echo_locked(char byte) {
    if (console_tty.termios.lflag & ECHO)
        emit_locked(byte);
}

void tty_init(void) {
    irq_spin_lock(&console_tty.lock);
    memset(&console_tty.termios, 0, sizeof(console_tty.termios));
    console_tty.termios.iflag = ICRNL;
    console_tty.termios.oflag = OPOST | ONLCR;
    console_tty.termios.cflag = CS8 | CREAD | CLOCAL;
    console_tty.termios.lflag = ICANON | ECHO | ECHOE | ECHOK;
    console_tty.termios.cc[VINTR] = 3;     /* Ctrl-C */
    console_tty.termios.cc[VQUIT] = 28;    /* Ctrl-\\ */
    console_tty.termios.cc[VERASE] = 127;  /* DEL */
    console_tty.termios.cc[VKILL] = 21;    /* Ctrl-U */
    console_tty.termios.cc[VEOF] = 4;      /* Ctrl-D */
    console_tty.termios.cc[VTIME] = 0;
    console_tty.termios.cc[VMIN] = 1;
    console_tty.termios.cc[VSTART] = 17;
    console_tty.termios.cc[VSTOP] = 19;
    console_tty.termios.cc[VSUSP] = 26;
    console_tty.termios.ibaud = 115200;
    console_tty.termios.obaud = 115200;
    console_tty.winsize = (struct tty_winsize) {
        .row = 25,
        .col = 80,
    };
    console_tty.canonical_length = 0;
    console_tty.canonical_offset = 0;
    console_tty.canonical_ready = false;
    console_tty.canonical_eof = false;
    irq_spin_unlock(&console_tty.lock);
}

static long read_noncanonical(char *buffer, size_t count) {
    irq_spin_lock(&console_tty.lock);
    size_t minimum = console_tty.termios.cc[VMIN];
    irq_spin_unlock(&console_tty.lock);
    if (minimum > count)
        minimum = count;

    size_t received = 0;
    while (received < minimum) {
        char raw = kbd_getc();
        irq_spin_lock(&console_tty.lock);
        bool discard;
        char byte = map_input_locked(raw, &discard);
        if (!discard) {
            echo_locked(byte);
            buffer[received++] = byte;
        }
        irq_spin_unlock(&console_tty.lock);
    }

    char raw;
    while (received < count && kbd_try_getc(&raw)) {
        irq_spin_lock(&console_tty.lock);
        bool discard;
        char byte = map_input_locked(raw, &discard);
        if (!discard) {
            echo_locked(byte);
            buffer[received++] = byte;
        }
        irq_spin_unlock(&console_tty.lock);
    }
    return (long)received;
}

static void fill_canonical_line(void) {
    for (;;) {
        char raw = kbd_getc();
        irq_spin_lock(&console_tty.lock);
        bool discard;
        char byte = map_input_locked(raw, &discard);
        if (discard) {
            irq_spin_unlock(&console_tty.lock);
            continue;
        }

        if ((uint8_t)byte == console_tty.termios.cc[VERASE]) {
            if (console_tty.canonical_length) {
                console_tty.canonical_length--;
                if ((console_tty.termios.lflag & (ECHO | ECHOE))
                        == (ECHO | ECHOE)) {
                    emit_locked('\b');
                    emit_locked(' ');
                    emit_locked('\b');
                } else {
                    echo_locked(byte);
                }
            }
            irq_spin_unlock(&console_tty.lock);
            continue;
        }

        if ((uint8_t)byte == console_tty.termios.cc[VKILL]) {
            while (console_tty.canonical_length) {
                console_tty.canonical_length--;
                if ((console_tty.termios.lflag & (ECHO | ECHOE))
                        == (ECHO | ECHOE)) {
                    emit_locked('\b');
                    emit_locked(' ');
                    emit_locked('\b');
                }
            }
            if ((console_tty.termios.lflag & (ECHO | ECHOK))
                    == (ECHO | ECHOK))
                emit_locked('\n');
            irq_spin_unlock(&console_tty.lock);
            continue;
        }

        if ((uint8_t)byte == console_tty.termios.cc[VEOF]) {
            console_tty.canonical_ready = true;
            console_tty.canonical_eof = console_tty.canonical_length == 0;
            irq_spin_unlock(&console_tty.lock);
            return;
        }

        if (console_tty.canonical_length < TTY_CANON_MAX)
            console_tty.canonical[console_tty.canonical_length++] = byte;
        echo_locked(byte);
        if (byte == '\n' || console_tty.canonical_length == TTY_CANON_MAX) {
            console_tty.canonical_ready = true;
            irq_spin_unlock(&console_tty.lock);
            return;
        }
        irq_spin_unlock(&console_tty.lock);
    }
}

long tty_read(void *buffer, size_t count) {
    if (!count)
        return 0;

    irq_spin_lock(&console_tty.lock);
    bool canonical = console_tty.termios.lflag & ICANON;
    irq_spin_unlock(&console_tty.lock);
    if (!canonical)
        return read_noncanonical(buffer, count);

    irq_spin_lock(&console_tty.lock);
    bool ready = console_tty.canonical_ready;
    irq_spin_unlock(&console_tty.lock);
    if (!ready)
        fill_canonical_line();

    irq_spin_lock(&console_tty.lock);
    if (console_tty.canonical_eof) {
        console_tty.canonical_eof = false;
        console_tty.canonical_ready = false;
        irq_spin_unlock(&console_tty.lock);
        return 0;
    }
    size_t available = console_tty.canonical_length - console_tty.canonical_offset;
    if (count > available)
        count = available;
    memcpy(buffer, console_tty.canonical + console_tty.canonical_offset, count);
    console_tty.canonical_offset += count;
    if (console_tty.canonical_offset == console_tty.canonical_length) {
        console_tty.canonical_length = 0;
        console_tty.canonical_offset = 0;
        console_tty.canonical_ready = false;
    }
    irq_spin_unlock(&console_tty.lock);
    return (long)count;
}

long tty_write(const void *buffer, size_t count) {
    const char *bytes = buffer;
    irq_spin_lock(&console_tty.lock);
    for (size_t i = 0; i < count; i++)
        emit_locked(bytes[i]);
    irq_spin_unlock(&console_tty.lock);
    return (long)count;
}

void tty_get_termios(struct tty_termios *out) {
    irq_spin_lock(&console_tty.lock);
    *out = console_tty.termios;
    irq_spin_unlock(&console_tty.lock);
}

void tty_set_termios(const struct tty_termios *termios, int flush_input) {
    irq_spin_lock(&console_tty.lock);
    console_tty.termios = *termios;
    if (flush_input) {
        console_tty.canonical_length = 0;
        console_tty.canonical_offset = 0;
        console_tty.canonical_ready = false;
        console_tty.canonical_eof = false;
    }
    irq_spin_unlock(&console_tty.lock);
    if (flush_input)
        kbd_flush_input();
}

void tty_get_winsize(struct tty_winsize *out) {
    irq_spin_lock(&console_tty.lock);
    *out = console_tty.winsize;
    irq_spin_unlock(&console_tty.lock);
}
