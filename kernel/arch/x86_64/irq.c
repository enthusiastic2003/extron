#include <arch/irh.h>
#include <arch/pic.h>
#include <arch/io.h>
#include <kernel/console.h>
#include <arch/irq.h>
#include <arch/irqs.h>


static irq_handler_fn irq_handlers[16];

void pic_eoi(uint8_t irq) {
    if (irq >= 8)
        outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

void irq_handler(struct isr_frame* f) {
    uint8_t irq = f->vector - 32;

    /*
     * Send EOI *before* dispatching.  The timer handler now calls
     * schedule() which may context-switch away from this stack —
     * if we haven't ACK'd the PIC yet, future IRQs on this line
     * will be masked until we come back, starving the timer.
     */
    pic_eoi(irq);

    if (irq_handlers[irq]) {
        irq_handlers[irq](f);
    }
}


void register_irq_handler(uint8_t irq, irq_handler_fn handler) {
    if (irq < 16) {
        irq_handlers[irq] = handler;
    }
}

void init_irq(){

    register_irq_handler(0, timer_handler);
    register_irq_handler(1, keyboard_handler);
}


