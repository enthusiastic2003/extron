#ifndef ARCH_CPU_H
#define ARCH_CPU_H

/* aarch64 counterpart to kernel/arch/x86_64/include/arch/cpu.h's `pause` —
 * yield is a hint that this core is spinning and another thread of
 * execution (SMT sibling, or just the branch predictor) may make better
 * use of the cycle. Required by kernel/include/kernel/sync/spinlock.h. */
static inline void cpu_relax(void) {
    __asm__ volatile ("yield");
}


#include <stdint.h>

/* IRQ mask helpers: save returns an opaque token for restore. */
static inline uint64_t arch_irq_save(void) {
    uint64_t v;
    __asm__ volatile ("mrs %0, daif" : "=r"(v));
    __asm__ volatile ("msr daifset, #3" ::: "memory");
    return v;
}
static inline void arch_irq_restore(uint64_t v) {
    __asm__ volatile ("msr daif, %0" :: "r"(v) : "memory");
}
/* Unmask IRQs and wait for one (leaves them unmasked). */
static inline void arch_wait_for_interrupt(void) {
    __asm__ volatile ("msr daifclr, #3\n\twfi" ::: "memory");
}
static inline void arch_set_thread_pointer(uint64_t v) {
    __asm__ volatile ("msr tpidr_el0, %0" :: "r"(v) : "memory");
}
static inline void arch_set_user_pagetable(uint64_t phys) {
    __asm__ volatile ("msr ttbr0_el1, %0" :: "r"(phys) : "memory");
    __asm__ volatile ("dsb ishst; tlbi vmalle1is; dsb ish; isb" ::: "memory");
}
static inline void arch_set_kernel_stack(uint64_t top) { (void)top; }
static inline void arch_memory_barrier_store(void) {
    __asm__ volatile ("dmb ishst" ::: "memory");
}

/* Drop to user mode at `entry` with the given stack; never returns. */
static inline __attribute__((noreturn))
void arch_enter_user(uint64_t entry, uint64_t sp, uint64_t argc, uint64_t argv) {
    register uint64_t x0 __asm__("x0") = argc;
    register uint64_t x1 __asm__("x1") = argv;
    register uint64_t x2 __asm__("x2") = entry;
    register uint64_t x3 __asm__("x3") = 0x0;   /* EL0t, all masks clear */
    register uint64_t x4 __asm__("x4") = sp;
    __asm__ volatile (
        "msr elr_el1, %2\n\t"
        "msr spsr_el1, %3\n\t"
        "msr sp_el0, %4\n\t"
        "eret"
        :: "r"(x0), "r"(x1), "r"(x2), "r"(x3), "r"(x4)
        : "memory"
    );
    __builtin_unreachable();
}

#endif
