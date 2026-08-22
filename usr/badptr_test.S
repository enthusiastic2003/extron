// Proves SYS_READ/SYS_WRITE reject pointers the calling process doesn't
// own. Before user_buffer_ok() (kernel/proc/syscall.c) all three of
// these succeeded: syscalls run at EL1 with TTBR1 live, so a raw user
// pointer handed straight to console_putc() could read the kernel's own
// half, and SYS_READ could write keystrokes into it.
//
// Each case prints PASS if the syscall returned -1 (rejected) and FAIL
// if it returned anything else — FAIL means the kernel really did the
// access, so the message itself is evidence of the hole.
.section ".rodata"
kw_pass:  .ascii "BADPTR write kernel-addr:   PASS (rejected)\n"
kw_pass_len = . - kw_pass
kw_fail:  .ascii "BADPTR write kernel-addr:   FAIL (accepted!)\n"
kw_fail_len = . - kw_fail

uw_pass:  .ascii "BADPTR write unmapped-addr: PASS (rejected)\n"
uw_pass_len = . - uw_pass
uw_fail:  .ascii "BADPTR write unmapped-addr: FAIL (accepted!)\n"
uw_fail_len = . - uw_fail

kr_pass:  .ascii "BADPTR read  kernel-addr:   PASS (rejected)\n"
kr_pass_len = . - kr_pass
kr_fail:  .ascii "BADPTR read  kernel-addr:   FAIL (accepted!)\n"
kr_fail_len = . - kr_fail

.section ".text"
.global _start
_start:
    // --- 1. SYS_WRITE pointed at the kernel half (0xFFFF800000000000,
    //        the HHDM base — real, live kernel mappings).
    mov     x8, #1                      // SYS_WRITE
    mov     x0, #1                      // fd = stdout
    movz    x1, #0xFFFF, lsl #48
    movk    x1, #0x8000, lsl #32
    mov     x2, #64
    svc     #0
    cmn     x0, #1                      // returned -1?
    b.eq    1f
    ldr     x1, =kw_fail
    mov     x2, #kw_fail_len
    b       2f
1:  ldr     x1, =kw_pass
    mov     x2, #kw_pass_len
2:  mov     x8, #1
    mov     x0, #1
    svc     #0

    // --- 2. SYS_WRITE pointed at an unmapped USER address. In range,
    //        so the range check alone wouldn't catch it — this is what
    //        the per-page mapped check is for. Without it the kernel
    //        takes a Data Abort at EL1, which panics: any process could
    //        halt the machine with one bad pointer.
    mov     x8, #1
    mov     x0, #1
    movz    x1, #0x0000, lsl #0
    movk    x1, #0x3000, lsl #16        // 0x30000000 — nothing mapped here
    mov     x2, #16
    svc     #0
    cmn     x0, #1
    b.eq    3f
    ldr     x1, =uw_fail
    mov     x2, #uw_fail_len
    b       4f
3:  ldr     x1, =uw_pass
    mov     x2, #uw_pass_len
4:  mov     x8, #1
    mov     x0, #1
    svc     #0

    // --- 3. SYS_READ aimed INTO the kernel half. The dangerous
    //        direction: this one has the kernel writing bytes we choose
    //        into its own memory. Must be rejected before blocking, so
    //        this returns immediately rather than waiting for a key.
    mov     x8, #0                      // SYS_READ
    mov     x0, #0                      // fd = stdin
    movz    x1, #0xFFFF, lsl #48
    movk    x1, #0x8000, lsl #32
    mov     x2, #1
    svc     #0
    cmn     x0, #1
    b.eq    5f
    ldr     x1, =kr_fail
    mov     x2, #kr_fail_len
    b       6f
5:  ldr     x1, =kr_pass
    mov     x2, #kr_pass_len
6:  mov     x8, #1
    mov     x0, #1
    svc     #0

    mov     x8, #7                      // SYS_EXIT
    mov     x0, #0
    svc     #0
7:  b       7b
