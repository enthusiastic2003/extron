#ifndef ARCH_IRQ_H
#define ARCH_IRQ_H
#include <arch/exceptions.h>

#define ARCH_IRQ_TIMER  0
#define ARCH_IRQ_SERIAL 4   /* COM1 */

void pic_remap(void);
void pic_unmask(unsigned irq);
void pic_eoi(unsigned irq);
static inline void arch_irq_enable_line(unsigned irq) { pic_unmask(irq); }

#endif
