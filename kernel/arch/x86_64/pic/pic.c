#include <stdint.h>
#include <arch/io.h>

/* PIC ports */
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

void pic_remap(void) {
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    /* start initialization */
    outb(PIC1_COMMAND, 0x11);
    outb(PIC2_COMMAND, 0x11);

    /* set vector offsets */
    outb(PIC1_DATA, 0x20); // IRQ0–7 → 32–39
    outb(PIC2_DATA, 0x28); // IRQ8–15 → 40–47

    /* tell master/slave wiring */
    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);

    /* environment info */
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);

    /* restore masks */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}