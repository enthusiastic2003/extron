#include <kernel/fbcon.h>
#include <kernel/drivers/fb.h>
#include <kernel/mm/kheap.h>
#include <kernel/klibc/string.h>
#include "font8x16.h"

#define FW 8
#define FH 16

struct cell { uint8_t ch; uint8_t fg, bg; uint8_t pad; };

static const struct framebuffer *fb;
static struct cell *cells;
static uint8_t *dirty;
static unsigned cols, rows;
static unsigned cx, cy;
static unsigned x_off, y_off;           /* centre the grid inside the screen */
static uint8_t cur_fg = 7, cur_bg = 0, bold;
static int reverse, cursor_visible = 1, wrap_pending;
static unsigned saved_cx, saved_cy;
static unsigned scroll_pending;
static unsigned drawn_cx, drawn_cy;
static int cursor_drawn;
static int active;

static uint32_t palette[16];
static const uint8_t vga_rgb[16][3] = {
    {0,0,0},{170,0,0},{0,170,0},{170,85,0},{0,0,170},{170,0,170},{0,170,170},{170,170,170},
    {85,85,85},{255,85,85},{85,255,85},{255,255,85},{85,85,255},{255,85,255},{85,255,255},{255,255,255},
};

/* ---- escape parser state ---- */
enum { ST_NORMAL, ST_ESC, ST_CSI };
static int state;
static int params[8], nparams;
static int priv;

static void mark(unsigned row) { dirty[row] = 1; }

static void clear_cells(unsigned row, unsigned c0, unsigned c1) {
    for (unsigned c = c0; c < c1; c++) {
        struct cell *p = &cells[row * cols + c];
        p->ch = ' '; p->fg = cur_fg; p->bg = cur_bg;
    }
    mark(row);
}

static void scroll_up(unsigned n) {
    if (n > rows) n = rows;
    memmove(cells, cells + n * cols, (rows - n) * cols * sizeof(struct cell));
    memmove(dirty, dirty + n, rows - n);
    for (unsigned r = rows - n; r < rows; r++) { clear_cells(r, 0, cols); }
    scroll_pending += n;
}

static void line_feed(void) {
    if (cy + 1 >= rows) scroll_up(1); else cy++;
}

static void put_glyph(unsigned c, uint8_t ch) {
    if (c >= cols) return;
    struct cell *p = &cells[cy * cols + c];
    uint8_t f = cur_fg, b = cur_bg;
    if (bold && f < 8) f += 8;
    if (reverse) { uint8_t t = f; f = b; b = t; }
    p->ch = ch; p->fg = f; p->bg = b;
    mark(cy);
}

static int param(int i, int def) { return (i < nparams && params[i] > 0) ? params[i] : def; }

static void sgr(void) {
    if (nparams == 0) { params[0] = 0; nparams = 1; }
    for (int i = 0; i < nparams; i++) {
        int p = params[i];
        if (p == 0) { cur_fg = 7; cur_bg = 0; bold = 0; reverse = 0; }
        else if (p == 1) bold = 1;
        else if (p == 22) bold = 0;
        else if (p == 7) reverse = 1;
        else if (p == 27) reverse = 0;
        else if (p >= 30 && p <= 37) cur_fg = (uint8_t)(p - 30);
        else if (p == 39) cur_fg = 7;
        else if (p >= 40 && p <= 47) cur_bg = (uint8_t)(p - 40);
        else if (p == 49) cur_bg = 0;
        else if (p >= 90 && p <= 97) cur_fg = (uint8_t)(p - 90 + 8);
        else if (p >= 100 && p <= 107) cur_bg = (uint8_t)(p - 100 + 8);
    }
}

static void csi(char final) {
    unsigned n;
    if (priv) {
        if (final == 'h' || final == 'l')
            if (nparams && params[0] == 25) { cursor_visible = final == 'h'; mark(cy); }
        return;
    }
    switch (final) {
    case 'A': n = param(0, 1); cy = cy > n ? cy - n : 0; break;
    case 'B': n = param(0, 1); cy = cy + n >= rows ? rows - 1 : cy + n; break;
    case 'C': n = param(0, 1); cx = cx + n >= cols ? cols - 1 : cx + n; break;
    case 'D': n = param(0, 1); cx = cx > n ? cx - n : 0; break;
    case 'G': n = param(0, 1); cx = n - 1 >= cols ? cols - 1 : n - 1; break;
    case 'd': n = param(0, 1); cy = n - 1 >= rows ? rows - 1 : n - 1; break;
    case 'H': case 'f': {
        unsigned r = param(0, 1), c = param(1, 1);
        cy = r - 1 >= rows ? rows - 1 : r - 1;
        cx = c - 1 >= cols ? cols - 1 : c - 1;
        break;
    }
    case 'J': {
        int m = nparams ? params[0] : 0;
        if (m == 0) { clear_cells(cy, cx, cols); for (unsigned r = cy + 1; r < rows; r++) clear_cells(r, 0, cols); }
        else if (m == 1) { for (unsigned r = 0; r < cy; r++) clear_cells(r, 0, cols); clear_cells(cy, 0, cx + 1); }
        else { for (unsigned r = 0; r < rows; r++) clear_cells(r, 0, cols); }
        break;
    }
    case 'K': {
        int m = nparams ? params[0] : 0;
        if (m == 0) clear_cells(cy, cx, cols);
        else if (m == 1) clear_cells(cy, 0, cx + 1);
        else clear_cells(cy, 0, cols);
        break;
    }
    case 'L': case 'M': {   /* insert / delete lines at cursor (shifts via repaint) */
        n = param(0, 1); if (n > rows - cy) n = rows - cy;
        struct cell *at = cells + cy * cols;
        if (final == 'L') {
            memmove(at + n * cols, at, (rows - cy - n) * cols * sizeof(struct cell));
            for (unsigned r = cy; r < cy + n; r++) clear_cells(r, 0, cols);
        } else {
            memmove(at, at + n * cols, (rows - cy - n) * cols * sizeof(struct cell));
            for (unsigned r = rows - n; r < rows; r++) clear_cells(r, 0, cols);
        }
        for (unsigned r = cy; r < rows; r++) mark(r);
        scroll_pending = rows;   /* force a full repaint of the shifted region */
        break;
    }
    case 'P': {             /* delete characters */
        n = param(0, 1); if (n > cols - cx) n = cols - cx;
        struct cell *row = cells + cy * cols;
        memmove(row + cx, row + cx + n, (cols - cx - n) * sizeof(struct cell));
        clear_cells(cy, cols - n, cols);
        break;
    }
    case '@': {             /* insert blanks */
        n = param(0, 1); if (n > cols - cx) n = cols - cx;
        struct cell *row = cells + cy * cols;
        memmove(row + cx + n, row + cx, (cols - cx - n) * sizeof(struct cell));
        clear_cells(cy, cx, cx + n);
        break;
    }
    case 'm': sgr(); break;
    case 's': saved_cx = cx; saved_cy = cy; break;
    case 'u': cx = saved_cx; cy = saved_cy; break;
    default: break;
    }
}

/* Boot messages printed before the framebuffer is up are kept and replayed. */
static char early_buf[32768];
static size_t early_len;

void fbcon_putc(char ch) {
    if (!active) {
        if (early_len < sizeof(early_buf)) early_buf[early_len++] = ch;
        return;
    }
    unsigned char c = (unsigned char)ch;
    mark(cy);   /* the cursor cell moves: repaint the row it leaves */

    if (state == ST_ESC) {
        if (c == '[') { state = ST_CSI; nparams = 0; priv = 0; memset(params, 0, sizeof(params)); return; }
        state = ST_NORMAL;
        if (c == 'c') { cur_fg = 7; cur_bg = 0; bold = reverse = 0; for (unsigned r = 0; r < rows; r++) clear_cells(r, 0, cols); cx = cy = 0; }
        else if (c == '7') { saved_cx = cx; saved_cy = cy; }
        else if (c == '8') { cx = saved_cx; cy = saved_cy; }
        else if (c == 'D') line_feed();
        else if (c == 'M') { if (cy) cy--; }
        return;
    }
    if (state == ST_CSI) {
        if (c >= '0' && c <= '9') {
            if (nparams == 0) nparams = 1;
            params[nparams - 1] = params[nparams - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (nparams == 0) nparams = 1;
            if (nparams < 8) nparams++;
        } else if (c == '?') priv = 1;
        else if (c >= 0x40 && c <= 0x7E) { state = ST_NORMAL; csi((char)c); }
        else if (c < 0x20 || c > 0x2F) state = ST_NORMAL;
        mark(cy);
        return;
    }

    switch (c) {
    case 0x1B: state = ST_ESC; return;
    case '\r': cx = 0; wrap_pending = 0; break;
    case '\n': line_feed(); wrap_pending = 0; break;
    case '\b': if (cx) cx--; wrap_pending = 0; break;
    case '\t': cx = (cx + 8) & ~7u; if (cx >= cols) cx = cols - 1; break;
    case 0x07: break;
    default:
        if (c < 0x20) break;
        if (wrap_pending) { cx = 0; line_feed(); wrap_pending = 0; }
        put_glyph(cx, c);
        if (cx + 1 >= cols) wrap_pending = 1; else cx++;
        break;
    }
    mark(cy);
}

void fbcon_write(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) fbcon_putc(buf[i]);
}

static void draw_cell(unsigned r, unsigned c, int invert) {
    const struct cell *p = &cells[r * cols + c];
    uint32_t fgc = palette[invert ? p->bg : p->fg & 15];
    uint32_t bgc = palette[invert ? p->fg & 15 : p->bg];
    const uint8_t *g = font8x16[p->ch];
    uint8_t *dst = (uint8_t *)fb->base + (y_off + r * FH) * fb->pitch + (x_off + c * FW) * 4;
    for (int y = 0; y < FH; y++) {
        uint32_t *px = (uint32_t *)dst;
        uint8_t bits = g[y];
        for (int x = 0; x < FW; x++) px[x] = (bits & (0x80 >> x)) ? fgc : bgc;
        dst += fb->pitch;
    }
}

static void erase_cursor(void) {
    if (cursor_drawn && drawn_cy < rows && drawn_cx < cols) draw_cell(drawn_cy, drawn_cx, 0);
    cursor_drawn = 0;
}

static int suspended;

void fbcon_flush(void) {
    if (!active || suspended) return;
    erase_cursor();

    if (scroll_pending) {
        if (scroll_pending < rows) {
            /* Shift the already-painted rows up in video memory; the rows
             * that now hold new text are marked dirty and repainted below. */
            uint8_t *base = (uint8_t *)fb->base + y_off * fb->pitch;
            size_t keep = (size_t)(rows - scroll_pending) * FH;
            memmove(base, base + (size_t)scroll_pending * FH * fb->pitch, keep * fb->pitch);
        } else {
            for (unsigned r = 0; r < rows; r++) dirty[r] = 1;
        }
        scroll_pending = 0;
    }
    for (unsigned r = 0; r < rows; r++) {
        if (!dirty[r]) continue;
        for (unsigned c = 0; c < cols; c++) draw_cell(r, c, 0);
        dirty[r] = 0;
    }
    if (cursor_visible) {
        draw_cell(cy, cx, 1);
        drawn_cx = cx; drawn_cy = cy; cursor_drawn = 1;
    }
}

void fbcon_suspend(void) {
    if (!active || suspended) return;
    erase_cursor();
    suspended = 1;
}

void fbcon_resume(void) {
    if (!active || !suspended) return;
    suspended = 0;
    cursor_drawn = 0;
    scroll_pending = 0;
    fb_fill(palette[0]);                       /* whatever the program left behind */
    for (unsigned r = 0; r < rows; r++) dirty[r] = 1;
    fbcon_flush();
}

int fbcon_active(void) { return active; }

void fbcon_get_size(unsigned *c, unsigned *r) { *c = cols; *r = rows; }

int fbcon_init(void) {
    fb = fb_get();
    if (!fb || fb->depth != 32 || fb->width < 80 * FW || fb->height < 25 * FH) return -1;
    cols = fb->width / FW;
    rows = fb->height / FH;
    x_off = (fb->width - cols * FW) / 2;
    y_off = (fb->height - rows * FH) / 2;
    cells = kmalloc(cols * rows * sizeof(struct cell));
    dirty = kmalloc(rows);
    if (!cells || !dirty) return -1;
    for (int i = 0; i < 16; i++)
        palette[i] = fb_rgb(vga_rgb[i][0], vga_rgb[i][1], vga_rgb[i][2]);
    fb_fill(palette[0]);
    memset(dirty, 0, rows);
    cx = cy = 0;
    for (unsigned r = 0; r < rows; r++) clear_cells(r, 0, cols);
    active = 1;
    size_t n = early_len;
    early_len = 0;
    for (size_t i = 0; i < n; i++) fbcon_putc(early_buf[i]);
    fbcon_flush();
    return 0;
}
