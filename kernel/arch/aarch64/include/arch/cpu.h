#ifndef ARCH_CPU_H
#define ARCH_CPU_H

/* aarch64 counterpart to kernel/arch/x86_64/include/arch/cpu.h's `pause` —
 * yield is a hint that this core is spinning and another thread of
 * execution (SMT sibling, or just the branch predictor) may make better
 * use of the cycle. Required by kernel/include/kernel/sync/spinlock.h. */
static inline void cpu_relax(void) {
    __asm__ volatile ("yield");
}

#endif
