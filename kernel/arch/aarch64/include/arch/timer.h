#ifndef ARCH_TIMER_H
#define ARCH_TIMER_H

/* Arms the ARM generic timer's non-secure physical timer (CNTP_TVAL_EL0/
 * CNTP_CTL_EL0) for a periodic tick at roughly `hz` Hz, and registers +
 * enables its GIC IRQ (kernel/arch/aarch64/gic.h's GIC_PPI_NS_PHYS_TIMER).
 * Call after gic_init(). */
void timer_init(unsigned hz);

#endif
