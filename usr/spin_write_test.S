// Phase 2 sleep/wake proof, proc B of a two-proc test (paired with
// usr/sleep_test.S, proc A). Writes a marker character 12 times, each
// separated by a busy-spin delay roughly matched to one 20Hz timer
// tick's worth of instructions, then exits. Runs the whole time proc A
// is asleep (about 1 second / ~20 ticks) — its markers should keep
// appearing on the console throughout that window if sleep() really
// yields the CPU rather than blocking the whole system.
.section ".rodata"
msg:
    .ascii "B\n"
msg_len = . - msg

.section ".text"
.global _start
_start:
    mov     x19, #12        // iteration count
loop:
    mov     x8, #1          // SYS_WRITE
    mov     x0, #1
    ldr     x1, =msg
    mov     x2, #msg_len
    svc     #0

    movz    x20, #0x8480, lsl #0   // busy-spin delay = 2,000,000
    movk    x20, #0x001E, lsl #16
delay:
    subs    x20, x20, #1
    b.ne    delay

    subs    x19, x19, #1
    b.ne    loop

    mov     x8, #7          // SYS_EXIT
    mov     x0, #0
    svc     #0
1:  b       1b
