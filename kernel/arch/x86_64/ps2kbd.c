#include <arch/irq.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/console.h>
#include "io.h"

/*
 * PS/2 keyboard (i8042, IRQ1, scancode set 1 as translated by the
 * controller). Translates key presses to the bytes a terminal would send
 * (US layout, Shift/Caps/Ctrl, arrows/Home/End/Del/PgUp/PgDn as VT100
 * sequences) and feeds them to the console tty exactly like serial input,
 * so the framebuffer console is fully interactive.
 */
static const char map_plain[0x3A] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
};
static const char map_shift[0x3A] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '
};

static int shift, ctrl, caps, ext;

static void put_seq(const char *s) { while (*s) kbd_inject_byte((uint8_t)*s++); }

static void handle_scancode(uint8_t sc) {
    if (sc == 0xE0) { ext = 1; return; }
    int release = sc & 0x80;
    uint8_t code = sc & 0x7F;
    int was_ext = ext; ext = 0;

    /* Every edge goes to the event ring (extended keys get bit 7), before the
     * ASCII translation below swallows modifiers and releases. */
    kbd_inject_event((uint8_t)(code | (was_ext ? 0x80 : 0)), !release);

    if (code == 0x2A || code == 0x36) { shift = !release; return; }
    if (code == 0x1D) { ctrl = !release; return; }
    if (release) return;
    if (code == 0x3A && !was_ext) { caps = !caps; return; }

    if (was_ext) {
        switch (code) {
        case 0x48: put_seq("\x1b[A"); break;
        case 0x50: put_seq("\x1b[B"); break;
        case 0x4D: put_seq("\x1b[C"); break;
        case 0x4B: put_seq("\x1b[D"); break;
        case 0x47: put_seq("\x1b[H"); break;
        case 0x4F: put_seq("\x1b[F"); break;
        case 0x53: put_seq("\x1b[3~"); break;
        case 0x49: put_seq("\x1b[5~"); break;
        case 0x51: put_seq("\x1b[6~"); break;
        case 0x1C: kbd_inject_byte('\n'); break;   /* keypad enter */
        }
        return;
    }
    if (code >= sizeof(map_plain)) return;
    char c = (shift ? map_shift : map_plain)[code];
    if (!c) return;
    int letter = (map_plain[code] >= 'a' && map_plain[code] <= 'z');
    if (letter && caps) c = shift ? map_plain[code] : map_shift[code];
    if (ctrl) {
        char p = map_plain[code];
        if (p >= 'a' && p <= 'z') c = p - 'a' + 1;
        else if (p == '[') c = 27;
        else if (p == '\\') c = 28;
        else if (p == ']') c = 29;
        else if (p == ' ') c = 0;
    }
    kbd_inject_byte((uint8_t)c);
}

static void ps2_irq(struct arch_frame *f) {
    (void)f;
    while (inb(0x64) & 1)
        handle_scancode(inb(0x60));
}

void ps2kbd_init(void) {
    while (inb(0x64) & 1) (void)inb(0x60);   /* drop anything pending */
    register_irq_handler(1, ps2_irq);
    pic_unmask(1);
}
