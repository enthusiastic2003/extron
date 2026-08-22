// SYS_UPTIME_MS proof, and a cross-check between the two independent
// time sources in the kernel.
//
// SYS_SLEEP is driven by the 20Hz timer interrupt (tick_count), while
// SYS_UPTIME_MS reads CNTPCT_EL0 directly. Sleeping a known 500ms and
// measuring it with the other clock validates both at once: if they
// disagree badly, either the tick-based sleep is miscounting or the
// counter-frequency maths in timer_uptime_ms() is wrong.
//
// The window is deliberately wide (400-800ms). 500ms is 10 ticks at
// 20Hz, so sleep granularity alone is +/-50ms, and the proc has to be
// rescheduled after waking while four other test procs are live. This
// is checking "the clock is real and roughly right", not calibration —
// a broken clock reads 0, or a constant, or wildly out.
.section ".data"
numbuf: .space 24

.section ".rodata"
lbl:      .ascii "UPTIME: slept 500ms, measured "
lbl_len = . - lbl
suffix:   .ascii " ms -> "
suffix_len = . - suffix
pass_msg: .ascii "PASS\n"
pass_len = . - pass_msg
fail_msg: .ascii "FAIL (outside 400-800ms)\n"
fail_len = . - fail_msg

.section ".text"
.global _start
_start:
    mov     x8, #10                     // SYS_UPTIME_MS
    svc     #0
    mov     x21, x0                     // t0

    mov     x8, #2                      // SYS_SLEEP, 500ms
    mov     x0, #0                      //   500,000,000 ns = 0x1DCD6500
    movz    x1, #0x6500
    movk    x1, #0x1DCD, lsl #16
    svc     #0

    mov     x8, #10                     // SYS_UPTIME_MS
    svc     #0
    sub     x22, x0, x21                // measured delta, ms

    ldr     x1, =lbl                    // "UPTIME: slept 500ms, measured "
    mov     x2, #lbl_len
    mov     x8, #1
    mov     x0, #1
    svc     #0

    mov     x0, x22                     // the measured number itself
    bl      print_dec

    ldr     x1, =suffix                 // " ms -> "
    mov     x2, #suffix_len
    mov     x8, #1
    mov     x0, #1
    svc     #0

    mov     x9, #400
    cmp     x22, x9
    b.lo    9f
    mov     x9, #800
    cmp     x22, x9
    b.hi    9f

    ldr     x1, =pass_msg
    mov     x2, #pass_len
    b       2f
9:
    ldr     x1, =fail_msg
    mov     x2, #fail_len
2:
    mov     x8, #1
    mov     x0, #1
    svc     #0

    mov     x8, #7                      // SYS_EXIT
    mov     x0, #0
    svc     #0
3:  b       3b

// Print x0 as unsigned decimal. Builds digits back-to-front from the end
// of numbuf, since that's the order division produces them, then writes
// the resulting run in one SYS_WRITE.
print_dec:
    mov     x23, x30                    // SYS_WRITE below clobbers nothing
    ldr     x10, =numbuf                // callee-saved, but bl did use x30
    add     x11, x10, #24               // one past the end
    mov     x12, #10
    mov     x13, #0                     // digit count
1:
    udiv    x14, x0, x12
    msub    x15, x14, x12, x0           // x15 = x0 - (x0/10)*10
    add     w15, w15, #'0'
    sub     x11, x11, #1
    strb    w15, [x11]
    add     x13, x13, #1
    mov     x0, x14
    cbnz    x0, 1b

    mov     x1, x11                     // first digit written
    mov     x2, x13
    mov     x8, #1                      // SYS_WRITE
    mov     x0, #1
    svc     #0

    mov     x30, x23
    ret
