// FP/SIMD context-switch proof, half A of a pair (see usr/fp_test_b.S,
// identical but for its fill pattern and its message).
//
// Both procs load a distinctive 64-bit pattern into the same FP
// registers, then yield repeatedly via SYS_SLEEP so the scheduler is
// forced to switch between them many times, re-checking after every
// wake. If context_switch (kernel/arch/aarch64/proc/switch.S) failed to
// save/restore FP state, each would come back holding the OTHER's
// pattern and both print FAIL.
//
// Register choice is deliberate. v8/v9 are AAPCS64 callee-saved, so a
// switch that only preserved d8-d15 would still pass on those. v0, v1
// and v31 are caller-saved, which the compiler is free to clobber
// across a call but which are absolutely live in a process preempted
// mid-instruction — those are the ones that catch a d8-d15-only save.
// Full 128-bit lanes are checked (d[0] on some, d[1] on others) so a
// save that only covered the low 64 bits is caught too.
.section ".rodata"
pass_msg: .ascii "FP_CTX A: PASS (v0/v1/v8/v9/v31 + tpidr_el0 intact, 20 switches)\n"
pass_len = . - pass_msg
fail_msg: .ascii "FP_CTX A: FAIL (FP or tpidr_el0 clobbered across switch)\n"
fail_len = . - fail_msg

.section ".text"
.global _start
_start:
    // x20 holds the expected pattern for the whole run: syscalls
    // clobber x0-x2, so it can't live there.
    movz    x20, #0xA1A1
    movk    x20, #0xA2A2, lsl #16
    movk    x20, #0xA3A3, lsl #32
    movk    x20, #0xA4A4, lsl #48

    dup     v0.2d,  x20
    dup     v1.2d,  x20
    dup     v8.2d,  x20
    dup     v9.2d,  x20
    dup     v31.2d, x20

    // TPIDR_EL0 too: same per-process-CPU-state question as the FP
    // registers, same pair-of-procs reason it needs two of us to test.
    // Written directly from EL0 — architecturally allowed, and the way
    // a real libc sets its thread pointer.
    msr     tpidr_el0, x20

    mov     x19, #20                    // iterations
1:
    mov     x8, #2                      // SYS_SLEEP, 50ms — long enough
    mov     x0, #0                      // that the other proc definitely
    movz    x1, #0xF080                 // runs in between (50,000,000ns)
    movk    x1, #0x02FA, lsl #16
    svc     #0

    umov    x1, v0.d[0]
    cmp     x1, x20
    b.ne    9f
    umov    x1, v1.d[1]
    cmp     x1, x20
    b.ne    9f
    umov    x1, v8.d[0]
    cmp     x1, x20
    b.ne    9f
    umov    x1, v9.d[1]
    cmp     x1, x20
    b.ne    9f
    umov    x1, v31.d[0]
    cmp     x1, x20
    b.ne    9f
    mrs     x1, tpidr_el0
    cmp     x1, x20
    b.ne    9f

    subs    x19, x19, #1
    b.ne    1b

    ldr     x1, =pass_msg
    mov     x2, #pass_len
    b       2f
9:
    ldr     x1, =fail_msg
    mov     x2, #fail_len
2:
    mov     x8, #1                      // SYS_WRITE
    mov     x0, #1
    svc     #0

    mov     x8, #7                      // SYS_EXIT
    mov     x0, #0
    svc     #0
3:  b       3b
