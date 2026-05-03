#include <stdarg.h>
#include <stdint.h>

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

static volatile char* vga = (volatile char*)0xB8000;

static int cursor_x = 0;
static int cursor_y = 0;

/* --- low-level --- */
static inline void putc_at(int x, int y, char c, char color) {
    int i = (y * VGA_WIDTH + x) * 2;
    vga[i]     = c;
    vga[i + 1] = color;
}

/* --- clear screen --- */
void clear_screen(char color) {
    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            putc_at(x, y, ' ', color);
        }
    }
    cursor_x = 0;
    cursor_y = 0;
}

/* --- scrolling --- */
static void scroll(void) {
    for (int y = 1; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            int from = (y * VGA_WIDTH + x) * 2;
            int to   = ((y - 1) * VGA_WIDTH + x) * 2;
            vga[to]     = vga[from];
            vga[to + 1] = vga[from + 1];
        }
    }

    for (int x = 0; x < VGA_WIDTH; x++) {
        putc_at(x, VGA_HEIGHT - 1, ' ', 0x0F);
    }

    cursor_y = VGA_HEIGHT - 1;
}

/* --- core output --- */
static void putc(char c, char color) {
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\t') {
        cursor_x = (cursor_x + 8) & ~(8 - 1);
    } else {
        putc_at(cursor_x, cursor_y, c, color);
        cursor_x++;
    }

    if (cursor_x >= VGA_WIDTH) {
        cursor_x = 0;
        cursor_y++;
    }

    if (cursor_y >= VGA_HEIGHT) {
        scroll();
    }
}

static void puts_col(const char* s, char color) {
    for (int i = 0; s[i]; i++) {
        putc(s[i], color);
    }
}

/* --- number helpers --- */
static void print_uint(uint64_t value, int base, char color) {
    char buf[32];
    int i = 0;

    if (value == 0) {
        putc('0', color);
        return;
    }

    while (value > 0) {
        int digit = value % base;
        buf[i++] = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
        value /= base;
    }

    while (i--) {
        putc(buf[i], color);
    }
}

static void print_int(int64_t value, char color) {
    if (value < 0) {
        putc('-', color);
        uint64_t abs = (uint64_t)(-(value + 1)) + 1;
        print_uint(abs, 10, color);
    } else {
        print_uint((uint64_t)value, 10, color);
    }
}

static void print_hex(uint64_t value, int width, char color) {
    for (int i = width - 1; i >= 0; i--) {
        int digit = (value >> (i * 4)) & 0xF;
        char c = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
        putc(c, color);
    }
}

/* --- printf core --- */
void kvprintf(const char* fmt, char color, va_list args) {
    for (int i = 0; fmt[i]; i++) {
        if (fmt[i] != '%') {
            putc(fmt[i], color);
            continue;
        }

        i++;

        switch (fmt[i]) {
            case 'c': {
                char c = (char)va_arg(args, int);
                putc(c, color);
                break;
            }

            case 's': {
                const char* s = va_arg(args, const char*);
                puts_col(s ? s : "(null)", color);
                break;
            }

            case 'd': {
                int val = va_arg(args, int);
                print_int(val, color);
                break;
            }

            case 'u': {
                unsigned int val = va_arg(args, unsigned int);
                print_uint(val, 10, color);
                break;
            }

            case 'x': {
                unsigned int val = va_arg(args, unsigned int);
                print_hex(val, 8, color);
                break;
            }

            case 'p': {
                uint64_t val = (uint64_t)va_arg(args, void*);
                puts_col("0x", color);
                print_hex(val, 16, color);
                break;
            }

            case '%':
                putc('%', color);
                break;

            default:
                putc('%', color);
                putc(fmt[i], color);
                break;
        }
    }
}

/* --- public API --- */
void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, 0x0F, args);
    va_end(args);
}

void kcprintf(const char* fmt, const char color, ...) {
    va_list args;
    va_start(args, color);
    kvprintf(fmt, color, args);
    va_end(args);
}

/* --- cursor control --- */
void set_cursor(int x, int y) {
    cursor_x = x;
    cursor_y = y;
}