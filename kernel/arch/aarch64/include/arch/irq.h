#ifndef ARCH_IRQ_H
#define ARCH_IRQ_H
#include <arch/gic.h>
#include <arch/exceptions.h>

/* The console UART's interrupt line, and enabling a line at the controller. */
#define ARCH_IRQ_SERIAL GIC_SPI_UART0
static inline void arch_irq_enable_line(unsigned id) { gic_enable_irq(id); }

#endif
