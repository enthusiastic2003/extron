#ifndef KERNEL_FBCON_H
#define KERNEL_FBCON_H

#include <stddef.h>

/*
 * Text console on the linear framebuffer (kernel/drivers/fb.h): 8x16 font,
 * 16-colour palette, scrolling, and the subset of ANSI/VT100 escapes that
 * BusyBox ash/vi/nano need (cursor movement, erase, SGR colours, line
 * insert/delete). It mirrors everything written to the serial console —
 * kernel messages via kprintf() and user tty output via the console tty.
 *
 * Drawing is deferred: fbcon_putc()/fbcon_write() only update a cell grid,
 * and fbcon_flush() paints the dirty rows (shifting the framebuffer when a
 * burst of output scrolled), so bulk output costs one repaint, not one per
 * character.
 */
int  fbcon_init(void);          /* 0 if a framebuffer exists and the console is live */
int  fbcon_active(void);
void fbcon_putc(char c);        /* '\n' is a bare line feed; '\r' returns the column */
void fbcon_write(const char *buf, size_t len);
void fbcon_flush(void);

#endif
