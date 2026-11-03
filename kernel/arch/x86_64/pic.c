#include <arch/irq.h>
#include "io.h"

/* Legacy 8259 PIC pair, remapped to vectors 0x20-0x2F. Every line starts
 * masked; drivers unmask their own via arch_irq_enable_line(). */
#define PIC1_CMD 0x20
#define PIC1_DAT 0x21
#define PIC2_CMD 0xA0
#define PIC2_DAT 0xA1

void pic_remap(void) {
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DAT, 0x20); io_wait();   /* master offset */
    outb(PIC2_DAT, 0x28); io_wait();   /* slave offset */
    outb(PIC1_DAT, 0x04); io_wait();   /* slave on IRQ2 */
    outb(PIC2_DAT, 0x02); io_wait();
    outb(PIC1_DAT, 0x01); io_wait();
    outb(PIC2_DAT, 0x01); io_wait();
    outb(PIC1_DAT, 0xFF);
    outb(PIC2_DAT, 0xFF);
}

void pic_unmask(unsigned irq) {
    if (irq < 8) {
        outb(PIC1_DAT, inb(PIC1_DAT) & ~(1u << irq));
    } else {
        outb(PIC1_DAT, inb(PIC1_DAT) & ~(1u << 2));
        outb(PIC2_DAT, inb(PIC2_DAT) & ~(1u << (irq - 8)));
    }
}

void pic_eoi(unsigned irq) {
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}
