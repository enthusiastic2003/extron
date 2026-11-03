#ifndef ARCH_CPU_H
#define ARCH_CPU_H

#include <stdint.h>

/* x86-64 counterpart of kernel/arch/aarch64/include/arch/cpu.h. */

#define MSR_EFER        0xC0000080
#define MSR_STAR        0xC0000081
#define MSR_LSTAR       0xC0000082
#define MSR_SFMASK      0xC0000084
#define MSR_FS_BASE     0xC0000100
#define MSR_GS_BASE     0xC0000101

#define X86_RFLAGS_IF   (1ULL << 9)

static inline void cpu_relax(void) {
    __asm__ volatile ("pause");
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"((uint32_t)v),
                      "d"((uint32_t)(v >> 32)) : "memory");
}

static inline uint64_t arch_irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void arch_irq_restore(uint64_t flags) {
    if (flags & X86_RFLAGS_IF)
        __asm__ volatile ("sti" ::: "memory");
    else
        __asm__ volatile ("cli" ::: "memory");
}

/* Unmask IRQs and halt until one arrives (leaves them unmasked). */
static inline void arch_wait_for_interrupt(void) {
    __asm__ volatile ("sti; hlt" ::: "memory");
}

static inline void arch_set_thread_pointer(uint64_t v) {
    wrmsr(MSR_FS_BASE, v);
}

static inline void arch_set_user_pagetable(uint64_t phys) {
    __asm__ volatile ("movq %0, %%cr3" :: "r"(phys) : "memory");
}

/* Where the next user->kernel transition (interrupt, syscall) lands. */
void arch_set_kernel_stack(uint64_t top);

static inline void arch_memory_barrier_store(void) {
    __asm__ volatile ("" ::: "memory"); /* x86 stores are already ordered */
}

/* Drop to ring 3 at `entry` with the given stack; never returns. */
void arch_enter_user(uint64_t entry, uint64_t sp, uint64_t argc, uint64_t argv)
    __attribute__((noreturn));

#endif
