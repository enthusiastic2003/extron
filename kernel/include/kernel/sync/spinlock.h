#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

/*
 * Single-core spinlock (no CLI).
 *
 * Uses atomic test-and-set for correctness and compiler/memory
 * barriers, but does NOT disable interrupts — timer ticks are
 * never lost.
 *
 * Rule: IRQ handlers must never acquire a lock that mainline
 * kernel code holds.  On a single core, that would deadlock.
 */

typedef struct spinlock {
    volatile int locked;    /* 0 = free, 1 = held */
} spinlock_t;

#define SPINLOCK_INIT { .locked = 0 }
/* Single-core: depth and saved IF live in file statics.
   Move to per-CPU state when SMP lands. */
static int lock_depth = 0;
static int saved_if   = 0;


static inline void spin_lock(spinlock_t *lock) {
    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        __asm__ volatile ("pause");
    }
}

static inline void spin_unlock(spinlock_t *lock) {
    __sync_lock_release(&lock->locked);
}

static inline int eflags_if(void) {
    uint64_t r;
    __asm__ volatile ("pushfq; pop %0" : "=r"(r));
    return (r >> 9) & 1;
}

static inline void irq_spin_lock(spinlock_t *lk) {
    int prev_if = eflags_if();
    __asm__ volatile ("cli");
    spin_lock(lk);
    if (lock_depth++ == 0)
        saved_if = prev_if;
}

static inline void irq_spin_unlock(spinlock_t *lk) {
    spin_unlock(lk);
    if (--lock_depth == 0 && saved_if)
        __asm__ volatile ("sti");
}
#endif /* SPINLOCK_H */
