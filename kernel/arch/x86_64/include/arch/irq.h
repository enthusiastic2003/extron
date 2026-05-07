#ifndef IRQ_H
#define IRQ_H
#include <arch/isr.h>
void irq_handler(struct isr_frame* f);
typedef void (*irq_handler_fn)(struct isr_frame*);
void register_irq_handler(uint8_t irq, irq_handler_fn handler);
void init_irq();
#endif