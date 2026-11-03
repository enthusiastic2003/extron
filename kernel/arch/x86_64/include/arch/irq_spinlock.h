#ifndef ARCH_IRQ_SPINLOCK_H
#define ARCH_IRQ_SPINLOCK_H

#include <kernel/sync/spinlock.h>
#include <stdint.h>

/*
 * IRQ-safe spinlock wrapper — x86-64 counterpart of the aarch64 one:
 * cli around the critical section, restoring the outermost saved RFLAGS.
 * Single core: depth and saved flags are file statics (see the aarch64
 * header for why per-translation-unit copies are still correct).
 */
static int      lock_depth  = 0;
static uint64_t saved_flags = 0;

static inline void irq_spin_lock(spinlock_t *lk) {
    uint64_t prev;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(prev) :: "memory");
    spin_lock(lk);
    if (lock_depth++ == 0)
        saved_flags = prev;
}

static inline void irq_spin_unlock(spinlock_t *lk) {
    spin_unlock(lk);
    if (--lock_depth == 0) {
        if (saved_flags & (1ULL << 9))
            __asm__ volatile ("sti" ::: "memory");
    }
}

#endif
