#ifndef CONSOLE_H
#define CONSOLE_H
#include <stdint.h>
// VGA color definitions (foreground/background)
enum vga_color {
    VGA_COLOR_BLACK         = 0x0,
    VGA_COLOR_BLUE          = 0x1,
    VGA_COLOR_GREEN         = 0x2,
    VGA_COLOR_CYAN          = 0x3,
    VGA_COLOR_RED           = 0x4,
    VGA_COLOR_MAGENTA       = 0x5,
    VGA_COLOR_BROWN         = 0x6,
    VGA_COLOR_LIGHT_GREY    = 0x7,
    VGA_COLOR_DARK_GREY     = 0x8,
    VGA_COLOR_LIGHT_BLUE    = 0x9,
    VGA_COLOR_LIGHT_GREEN   = 0xA,
    VGA_COLOR_LIGHT_CYAN    = 0xB,
    VGA_COLOR_LIGHT_RED     = 0xC,
    VGA_COLOR_LIGHT_MAGENTA = 0xD,
    VGA_COLOR_YELLOW        = 0xE,
    VGA_COLOR_WHITE         = 0xF,
};

// Helper to combine foreground + background
static inline char vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return fg | (bg << 4);
}

void console_putc(char c);
void kprintf(const char* s, ...);
void kcprintf(const char* s, const char color, ...);
void kvprintf(const char* fmt, char color, ...);
void clear_screen(char color);
void set_cursor(int x, int y);
void set_console_write_loc(uint64_t vga_offset);

#endif