#ifndef ARCH_IRQ_SPINLOCK_H
#define ARCH_IRQ_SPINLOCK_H

#include <kernel/sync/spinlock.h>

/*
 * IRQ-safe spinlock wrapper: masks interrupts (cli/sti) around the
 * critical section. x86-only — no aarch64 equivalent yet since nothing
 * enables interrupts there (Milestone 4: GIC/exceptions).
 *
 * Single-core: depth and saved IF live in file statics.
 * Move to per-CPU state when SMP lands.
 */
static int lock_depth = 0;
static int saved_if   = 0;

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

#endif
