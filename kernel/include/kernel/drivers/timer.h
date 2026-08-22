#ifndef KERNEL_DRIVERS_TIMER_H
#define KERNEL_DRIVERS_TIMER_H

#include <kernel/mm/pmm.h>

/* Arms the ARM generic timer's non-secure physical timer (CNTP_TVAL_EL0/
 * CNTP_CTL_EL0) for a periodic tick at roughly `hz` Hz, and registers +
 * enables its GIC IRQ (kernel/arch/aarch64/include/arch/gic.h's
 * GIC_PPI_NS_PHYS_TIMER). Call after gic_init(). */
void timer_init(unsigned hz);

/* Debug-only: watch two physical addresses (kernel_aarch64.c's 2-process
 * scheduler test's shared counter pages) and print their contents every
 * ~20 ticks. Pass 0, 0 to disable. */
void timer_set_counter_watch(phys_addr_t a, phys_addr_t b);

/* Monotonic tick counter and configured rate — used by SYS_SLEEP
 * (kernel/proc/syscall.c) to convert a requested duration into a
 * deadline in ticks, and by proc_wakeup_expired() callers to compare
 * against it. */
uint64_t timer_ticks(void);
unsigned timer_ticks_per_second(void);

#endif
