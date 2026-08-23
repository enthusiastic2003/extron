// void context_switch(struct cpu_context *old, struct cpu_context *new);
//                      x0                       x1
//
// aarch64 counterpart to kernel/arch/x86_64/proc/switch.asm — but
// simpler: x86 has to fabricate a synthetic ".resume" label and store
// its address as the saved "rip", because call/ret communicate the
// return address via the stack, not a register — a plain ret after
// swapping rsp would land on whatever's at the new stack, not back at
// the right C call site. AArch64's bl/ret convention already keeps the
// return address in a normal register (x30/lr) for the whole call — one
// already in the callee-saved set below — so no stub is needed: saving
// and restoring lr like any other register, then a plain `ret`, is
// already correct.
//
// struct cpu_context field offsets (kernel/include/kernel/proc/proc.h):
//   x19..x28 = 0x00..0x48   fp = 0x50   lr = 0x58   sp = 0x60
//   fpcr = 0x68   fpsr = 0x70   tpidr_el0 = 0x78   v0..v31 = 0x80..0x27F
//
// The FP/SIMD half saves ALL of v0-v31, not just AAPCS64's callee-saved
// d8-d15. This isn't preserving a calling convention across a call —
// it's preserving a user process's live FP state across preemption,
// and preemption lands on an arbitrary instruction rather than a call
// boundary, so every register is potentially live.
//
// Sound only because the kernel is built -mgeneral-regs-only: exception
// entry never saves v0-v31, so those registers still hold the outgoing
// process's own values right up to this point. If kernel code could
// touch FP, this would be saving the kernel's scratch instead and the
// save would have to move up into SAVE_CONTEXT.

.section ".text"
.global context_switch
context_switch:
    stp x19, x20, [x0, #0x00]
    stp x21, x22, [x0, #0x10]
    stp x23, x24, [x0, #0x20]
    stp x25, x26, [x0, #0x30]
    stp x27, x28, [x0, #0x40]
    stp x29, x30, [x0, #0x50]
    mov x9, sp
    str x9,       [x0, #0x60]

    mrs x10, fpcr
    mrs x11, fpsr
    stp x10, x11, [x0, #0x68]

    // TPIDR_EL0 (thread pointer): per-process, EL0-writable, and never
    // touched by the kernel — so like the FP state below, the register
    // still holds the OUTGOING process's value at this point. x12 is
    // caller-saved scratch, free to use here.
    mrs x12, tpidr_el0
    str x12,      [x0, #0x78]

    stp q0,  q1,  [x0, #0x080]
    stp q2,  q3,  [x0, #0x0A0]
    stp q4,  q5,  [x0, #0x0C0]
    stp q6,  q7,  [x0, #0x0E0]
    stp q8,  q9,  [x0, #0x100]
    stp q10, q11, [x0, #0x120]
    stp q12, q13, [x0, #0x140]
    stp q14, q15, [x0, #0x160]
    stp q16, q17, [x0, #0x180]
    stp q18, q19, [x0, #0x1A0]
    stp q20, q21, [x0, #0x1C0]
    stp q22, q23, [x0, #0x1E0]
    stp q24, q25, [x0, #0x200]
    stp q26, q27, [x0, #0x220]
    stp q28, q29, [x0, #0x240]
    stp q30, q31, [x0, #0x260]

    ldp x19, x20, [x1, #0x00]
    ldp x21, x22, [x1, #0x10]
    ldp x23, x24, [x1, #0x20]
    ldp x25, x26, [x1, #0x30]
    ldp x27, x28, [x1, #0x40]
    ldp x29, x30, [x1, #0x50]
    ldr x9,       [x1, #0x60]
    mov sp, x9

    ldp x10, x11, [x1, #0x68]
    msr fpcr, x10
    msr fpsr, x11

    ldr x12,      [x1, #0x78]
    msr tpidr_el0, x12

    ldp q0,  q1,  [x1, #0x080]
    ldp q2,  q3,  [x1, #0x0A0]
    ldp q4,  q5,  [x1, #0x0C0]
    ldp q6,  q7,  [x1, #0x0E0]
    ldp q8,  q9,  [x1, #0x100]
    ldp q10, q11, [x1, #0x120]
    ldp q12, q13, [x1, #0x140]
    ldp q14, q15, [x1, #0x160]
    ldp q16, q17, [x1, #0x180]
    ldp q18, q19, [x1, #0x1A0]
    ldp q20, q21, [x1, #0x1C0]
    ldp q22, q23, [x1, #0x1E0]
    ldp q24, q25, [x1, #0x200]
    ldp q26, q27, [x1, #0x220]
    ldp q28, q29, [x1, #0x240]
    ldp q30, q31, [x1, #0x260]

    ret

// void cpu_context_save_fpsimd(struct cpu_context *c);
//                              x0
//
// Snapshot the FP/SIMD registers, FPCR/FPSR and TPIDR_EL0 that are LIVE
// in the hardware right now into `c`. The save half of context_switch,
// on its own.
//
// fork() needs this because the parent's struct cpu_context is stale by
// definition: it holds whatever was saved at its last switch-OUT, and
// the parent has been running ever since. The current values are in the
// registers, not in the struct — the kernel is -mgeneral-regs-only, so
// nothing between the `svc` and here has disturbed them. Writing them
// straight into the CHILD's context is what makes a forked process
// inherit the FP and TLS state a fork is supposed to preserve.
//
// Same offsets as context_switch above, and they are checked by the
// _Static_asserts in kernel/proc/proc.c.
.global cpu_context_save_fpsimd
cpu_context_save_fpsimd:
    mrs x10, fpcr
    mrs x11, fpsr
    stp x10, x11, [x0, #0x68]

    mrs x12, tpidr_el0
    str x12,      [x0, #0x78]

    stp q0,  q1,  [x0, #0x080]
    stp q2,  q3,  [x0, #0x0A0]
    stp q4,  q5,  [x0, #0x0C0]
    stp q6,  q7,  [x0, #0x0E0]
    stp q8,  q9,  [x0, #0x100]
    stp q10, q11, [x0, #0x120]
    stp q12, q13, [x0, #0x140]
    stp q14, q15, [x0, #0x160]
    stp q16, q17, [x0, #0x180]
    stp q18, q19, [x0, #0x1A0]
    stp q20, q21, [x0, #0x1C0]
    stp q22, q23, [x0, #0x1E0]
    stp q24, q25, [x0, #0x200]
    stp q26, q27, [x0, #0x220]
    stp q28, q29, [x0, #0x240]
    stp q30, q31, [x0, #0x260]
    ret
