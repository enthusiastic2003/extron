#ifndef ARCH_IRQ_SPINLOCK_H
#define ARCH_IRQ_SPINLOCK_H

#include <kernel/sync/spinlock.h>
#include <stdint.h>

/*
 * IRQ-safe spinlock wrapper — aarch64 counterpart to
 * kernel/arch/x86_64/include/arch/irq_spinlock.h. That file's own comment
 * noted aarch64 had "nothing to mask" yet; that stopped being true once
 * GIC/timer bring-up landed. Masks DAIF.I and DAIF.F around the critical
 * section (matching exceptions_enable_irqs()'s choice to unmask both —
 * GIC-400's Group 0 default routes to FIQ, not IRQ) instead of x86's
 * pushfq/cli/sti.
 *
 * Single-core: depth and saved DAIF live in file statics, same as x86's
 * version. Move to per-CPU state when SMP lands.
 */
static int      lock_depth  = 0;
static uint64_t saved_daif  = 0;

static inline uint64_t read_daif(void) {
    uint64_t v;
    __asm__ volatile ("mrs %0, daif" : "=r"(v));
    return v;
}

static inline void irq_spin_lock(spinlock_t *lk) {
    uint64_t prev_daif = read_daif();
    __asm__ volatile ("msr daifset, #3");
    spin_lock(lk);
    if (lock_depth++ == 0)
        saved_daif = prev_daif;
}

static inline void irq_spin_unlock(spinlock_t *lk) {
    spin_unlock(lk);
    if (--lock_depth == 0)
        __asm__ volatile ("msr daif, %0" :: "r"(saved_daif) : "memory");
}

#endif
