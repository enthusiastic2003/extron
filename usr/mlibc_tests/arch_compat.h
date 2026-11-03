#ifndef EXTRON_TEST_ARCH_COMPAT_H
#define EXTRON_TEST_ARCH_COMPAT_H
/* Per-architecture helpers shared by the mlibc test programs. */
#include <stdint.h>

#if defined(__aarch64__)
static inline long extron_raw_syscall(long n, long a1, long a2, long a3) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a1;
    register long x1 __asm__("x1") = a2;
    register long x2 __asm__("x2") = a3;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}
/* Fault by loading from `address` (kernel must deliver SIGSEGV). */
#define EXTRON_FAULT_LOAD(address) \
    __asm__ volatile ("ldr x9, [%0]" :: "r"(address) : "x9", "memory")
#define EXTRON_MC_PC(uc) ((uc)->uc_mcontext.pc)
#define EXTRON_READ_TP(var) __asm__ volatile ("mrs %0, tpidr_el0" : "=r"(var))
#elif defined(__x86_64__)
static inline long extron_raw_syscall(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                      : "rcx", "r11", "memory", "cc");
    return r;
}
#define EXTRON_FAULT_LOAD(address) \
    __asm__ volatile ("movq (%0), %%rax" :: "r"(address) : "rax", "memory")
#define EXTRON_MC_PC(uc) ((uc)->uc_mcontext.gregs[REG_RIP])
/* x86-64: FS:0 holds the Tcb self pointer, i.e. the FS base. */
#define EXTRON_READ_TP(var) __asm__ volatile ("movq %%fs:0, %0" : "=r"(var))
#else
#error "unsupported architecture"
#endif

#endif
