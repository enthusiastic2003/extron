#ifndef ARCH_CONTEXT_H
#define ARCH_CONTEXT_H

#include <stdint.h>

/*
 * aarch64 callee-saved register set (AAPCS64: x19-x28, fp, lr, sp) —
 * saved/restored by context_switch (kernel/arch/aarch64/proc/switch.S).
 * Field order must match that file's hardcoded offsets:
 *   x19..x28 = 0x00..0x48 (8 regs, 8 bytes each)
 *   fp = 0x50   lr = 0x58   sp = 0x60
 */
/*
 * Saved by context_switch (kernel/arch/aarch64/proc/switch.S). Field
 * order and offsets are load-bearing — that file hardcodes them.
 *
 * The FP/SIMD half is not the usual AAPCS64 callee-saved subset
 * (d8-d15). It is ALL of v0-v31, plus FPCR/FPSR, because what's being
 * preserved here isn't a compiler calling convention — it's a user
 * process's live floating-point state, and a process gets preempted at
 * an arbitrary instruction, not at a call boundary.
 *
 * That works because the kernel is built -mgeneral-regs-only (see the
 * Makefile's comment), so it provably never touches FP. Exception entry
 * doesn't save v0-v31 and doesn't need to: the registers still hold the
 * interrupted process's own values all the way down to here, which is
 * exactly what makes this the right place to swap them.
 *
 * Aligned to 16 so the q-register stp/ldp pairs land on aligned
 * addresses. Not strictly required — boot.S leaves SCTLR_EL1.A clear,
 * so unaligned SIMD accesses are permitted — but free to get right.
 */
struct cpu_context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28; /* 0x00 */
    uint64_t fp;                                               /* 0x50 */
    uint64_t lr;                                               /* 0x58 */
    uint64_t sp;                                               /* 0x60 */
    uint64_t fpcr;                                             /* 0x68 */
    uint64_t fpsr;                                             /* 0x70 */
    /* TPIDR_EL0 — the thread pointer. Architecturally EL0-writable
     * (that's why sys_tcb_set() notes it may never be called), and
     * per-thread by definition, so it belongs here for exactly the same
     * reason the FP registers do: the kernel never touches it, so its
     * value at this point is still the outgoing process's own.
     *
     * Nothing uses TLS yet, which is the only reason this wasn't already
     * a bug — the same way FP/SIMD and SP_EL0 were both invisible until
     * a workload happened to use the register. Fixed ahead of mlibc
     * rather than after it, since the symptom would be every thread
     * seeing another process's TLS base.
     *
     * Sits in what used to be pure alignment padding, so the struct's
     * size and v[]'s offset are unchanged. */
    uint64_t tpidr_el0;                   /* 0x78 */
    uint64_t v[64];                       /* 0x80: v0-v31, 128 bits each */
} __attribute__((aligned(16)));


#include <stddef.h>
/* kernel/arch/aarch64/proc/switch.S hardcodes these offsets. A silent
 * mismatch would corrupt FP state across every context switch in a way
 * that only shows up as wrong pixels or wrong arithmetic much later, so
 * fail the build instead. */
_Static_assert(offsetof(struct cpu_context, sp)   == 0x60, "switch.S: sp offset");
_Static_assert(offsetof(struct cpu_context, fpcr) == 0x68, "switch.S: fpcr offset");
_Static_assert(offsetof(struct cpu_context, fpsr) == 0x70, "switch.S: fpsr offset");
_Static_assert(offsetof(struct cpu_context, tpidr_el0) == 0x78, "switch.S: tpidr_el0 offset");
_Static_assert(offsetof(struct cpu_context, v)    == 0x80, "switch.S: v[] offset");
_Static_assert(sizeof(((struct cpu_context *)0)->v) == 32 * 16, "switch.S: v[] covers v0-v31");

/* First-launch / fork-resume context setup: as if the thread had already been
 * switched out once, with `entry` as the resume point on a fresh stack. */
static inline void arch_context_init(struct cpu_context *c, uint64_t sp,
                                     void (*entry)(void), uint64_t tls) {
    *c = (struct cpu_context){0};
    c->lr = (uint64_t)entry;
    c->sp = sp;
    c->tpidr_el0 = tls;
}

/* Re-point an already-initialised context (fork child) at a trap frame. */
static inline void arch_context_set_resume(struct cpu_context *c, uint64_t sp,
                                           void (*entry)(void)) {
    c->sp = sp;
    c->lr = (uint64_t)entry;
}

#endif
