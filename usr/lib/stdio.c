#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <extron/syscall.h>

/*
 * A single vsnprintf everything else routes through, so format handling
 * exists once. Supports what DOOM actually uses: %d %i %u %x %X %c %s %p
 * %% plus a field width, zero-padding, left-align, and the l/ll length
 * modifiers. No floating point — DOOM's renderer is fixed-point, and a
 * float formatter is a lot of subtle code to get wrong for no caller.
 */

struct sink {
    char  *buf;
    size_t cap;   /* bytes writable, excluding the terminator */
    size_t len;   /* bytes that WOULD have been written (C99 return) */
};

static void emit(struct sink *s, char c) {
    if (s->buf && s->len < s->cap) s->buf[s->len] = c;
    s->len++;
}

static void emit_str(struct sink *s, const char *p, size_t n) {
    while (n--) emit(s, *p++);
}

static void emit_pad(struct sink *s, char pad, int n) {
    while (n-- > 0) emit(s, pad);
}

/* Digits come out of division in reverse, so build into a scratch buffer
 * and emit backwards. 24 bytes covers 64-bit in base 10 with room over. */
static int utoa(char *out, uint64_t v, unsigned base, int upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do { out[n++] = digits[v % base]; v /= base; } while (v);
    return n;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct sink s = { buf, size ? size - 1 : 0, 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(&s, *fmt); continue; }
        fmt++;

        int left = 0, zero = 0, width = 0, longs = 0;
        for (;; fmt++) {
            if (*fmt == '-')      left = 1;
            else if (*fmt == '0') zero = 1;
            else break;
        }
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else for (; *fmt >= '0' && *fmt <= '9'; fmt++) width = width * 10 + (*fmt - '0');

        /* Precision. For integers this is a MINIMUM digit count,
         * zero-filled — DOOM builds its HUD font lump names with
         * "STCFN%.3d", so without this it looks up a lump literally
         * called STCFN%.3d and dies. For strings it is a maximum. */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else for (; *fmt >= '0' && *fmt <= '9'; fmt++) prec = prec * 10 + (*fmt - '0');
        }

        for (; *fmt == 'l'; fmt++) longs++;
        if (*fmt == 'z') { longs = 1; fmt++; }

        char scratch[24];
        int  n = 0;
        const char *str = scratch;
        char sign = 0;

        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = longs ? va_arg(ap, int64_t) : va_arg(ap, int);
            uint64_t mag;
            if (v < 0) { sign = '-'; mag = (uint64_t)(-(v + 1)) + 1; } /* INT64_MIN-safe */
            else       { mag = (uint64_t)v; }
            n = utoa(scratch, mag, 10, 0);
            break;
        }
        case 'u':
            n = utoa(scratch, longs ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10, 0);
            break;
        case 'x':
            n = utoa(scratch, longs ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, 0);
            break;
        case 'X':
            n = utoa(scratch, longs ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, 1);
            break;
        case 'p':
            emit_str(&s, "0x", 2);
            n = utoa(scratch, (uint64_t)(uintptr_t)va_arg(ap, void *), 16, 0);
            break;
        case 'c':
            scratch[0] = (char)va_arg(ap, int);
            str = scratch; n = 1;
            goto padded_str;
        case 's': {
            str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            n = (int)strlen(str);
            /* Precision truncates a string rather than padding it. */
            if (prec >= 0 && prec < n) n = prec;
            goto padded_str;
        }
        case '%':
            emit(&s, '%');
            continue;
        default:
            /* Unknown specifier: emit it literally rather than silently
             * swallowing it, so a typo is visible instead of vanishing. */
            emit(&s, '%');
            emit(&s, *fmt);
            continue;
        }

        /* Numeric: digits sit reversed in scratch, so emit backwards.
         * Pad against this conversion's own width, not s.len — s.len is
         * the running total for the whole format string, so using it
         * here made left-justified numerics pad by a wildly wrong amount
         * as soon as anything preceded them. */
        int zeros   = (prec > n) ? prec - n : 0;
        int emitted = n + zeros + (sign ? 1 : 0);
        /* A precision suppresses the '0' flag, per C: "%.3d" pads with
         * zeros to 3 digits, then any remaining width with spaces. */
        if (!left) emit_pad(&s, (zero && prec < 0) ? '0' : ' ', width - emitted);
        if (sign) emit(&s, sign);
        emit_pad(&s, '0', zeros);
        while (n--) emit(&s, scratch[n]);
        if (left) emit_pad(&s, ' ', width - emitted);
        continue;

    padded_str:
        if (!left) emit_pad(&s, ' ', width - n);
        emit_str(&s, str, (size_t)n);
        if (left) emit_pad(&s, ' ', width - n);
    }

    if (buf && size) buf[s.len < s.cap ? s.len : s.cap] = '\0';
    return (int)s.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

int vprintf(const char *fmt, va_list ap) {
    char buf[512];
    int r = vsnprintf(buf, sizeof buf, fmt, ap);
    size_t n = (size_t)r < sizeof buf - 1 ? (size_t)r : sizeof buf - 1;
    sys_write(1, buf, n);
    return r;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int puts(const char *s) {
    sys_write(1, s, strlen(s));
    sys_write(1, "\n", 1);
    return 0;
}

int putchar(int c) {
    char ch = (char)c;
    sys_write(1, &ch, 1);
    return c;
}
