#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>
#include <arch/cpu.h>

/*
 * Single-core spinlock (no CLI).
 *
 * Uses atomic test-and-set for correctness and compiler/memory
 * barriers, but does NOT disable interrupts — timer ticks are
 * never lost.
 *
 * Rule: IRQ handlers must never acquire a lock that mainline
 * kernel code holds.  On a single core, that would deadlock.
 *
 * For an IRQ-safe variant that also masks interrupts, see
 * kernel/arch/x86_64/include/arch/irq_spinlock.h (x86-only for now —
 * aarch64 has nothing to mask until Milestone 4's GIC/exceptions land).
 */

typedef struct spinlock {
    volatile int locked;    /* 0 = free, 1 = held */
} spinlock_t;

#define SPINLOCK_INIT { .locked = 0 }

static inline void spin_lock(spinlock_t *lock) {
    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        cpu_relax();
    }
}

static inline void spin_unlock(spinlock_t *lock) {
    __sync_lock_release(&lock->locked);
}

#endif /* SPINLOCK_H */
