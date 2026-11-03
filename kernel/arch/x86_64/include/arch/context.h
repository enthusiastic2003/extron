#ifndef ARCH_CONTEXT_H
#define ARCH_CONTEXT_H

#include <stdint.h>
#include <stddef.h>
#include <kernel/klibc/string.h>

/*
 * Saved by context_switch (kernel/arch/x86_64/proc/switch.S); the offsets
 * below are load-bearing. SysV callee-saved registers + rsp, the user's
 * FS base (TLS pointer — per-thread, never touched by the kernel, hence
 * saved exactly like aarch64's TPIDR_EL0), and the full x87/SSE state via
 * FXSAVE. The kernel is built -mgeneral-regs-only so it never clobbers
 * XMM/x87 state itself: at switch time the registers still hold the
 * outgoing thread's own values.
 */
struct cpu_context {
    uint64_t rbx;       /* 0x00 */
    uint64_t rbp;       /* 0x08 */
    uint64_t r12;       /* 0x10 */
    uint64_t r13;       /* 0x18 */
    uint64_t r14;       /* 0x20 */
    uint64_t r15;       /* 0x28 */
    uint64_t rsp;       /* 0x30 */
    uint64_t fs_base;   /* 0x38 */
    uint8_t  fxsave[512] __attribute__((aligned(16))); /* 0x40 */
} __attribute__((aligned(16)));

_Static_assert(offsetof(struct cpu_context, rsp) == 0x30, "switch.S: rsp offset");
_Static_assert(offsetof(struct cpu_context, fs_base) == 0x38, "switch.S: fs_base offset");
_Static_assert(offsetof(struct cpu_context, fxsave) == 0x40, "switch.S: fxsave offset");

/* Default x87/SSE state: FCW=0x37F (all x87 exceptions masked, extended
 * precision), MXCSR=0x1F80 (all SSE exceptions masked). A zeroed FXSAVE
 * image would instead unmask every FP exception. */
static inline void arch_context_default_fpu(struct cpu_context *c) {
    uint16_t fcw = 0x037F;
    uint32_t mxcsr = 0x1F80;
    memcpy(&c->fxsave[0], &fcw, sizeof(fcw));
    memcpy(&c->fxsave[24], &mxcsr, sizeof(mxcsr));
}

/* First-launch context: `ret` from context_switch lands in `entry` on a
 * fresh stack, as if the thread had already been switched out once. The
 * return slot sits 16 bytes below `sp` so entry starts with the SysV
 * "rsp == 8 mod 16" alignment of a normal call. */
static inline void arch_context_init(struct cpu_context *c, uint64_t sp,
                                     void (*entry)(void), uint64_t tls) {
    memset(c, 0, sizeof(*c));
    arch_context_default_fpu(c);
    *(uint64_t *)(sp - 16) = (uint64_t)entry;
    c->rsp = sp - 16;
    c->fs_base = tls;
}

/* Re-point an initialised context (fork child) so `ret` lands in `entry`
 * with rsp == sp (entry is expected to pop a trap frame from there). */
static inline void arch_context_set_resume(struct cpu_context *c, uint64_t sp,
                                           void (*entry)(void)) {
    *(uint64_t *)(sp - 8) = (uint64_t)entry;
    c->rsp = sp - 8;
}

#endif
