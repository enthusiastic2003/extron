// Phase 2 sleep/wake proof, proc A of a two-proc test (paired with
// usr/spin_write_test.S, proc B). Calls the real SYS_SLEEP syscall for
// 1 second, then SYS_WRITEs a marker, then exits via SYS_EXIT. If
// sleep() genuinely yields the CPU (kernel/proc/proc.c), proc B's own
// writes should keep appearing on the console during A's 1-second
// sleep window, and "A: woke up" should land only after roughly 20
// ticks (20Hz, kernel_aarch64.c's timer_init(20)) have passed.
.section ".rodata"
before_msg:
    .ascii "A: sleeping for 1s...\n"
before_len = . - before_msg
after_msg:
    .ascii "A: woke up\n"
after_len = . - after_msg

.section ".text"
.global _start
_start:
    mov     x8, #1          // SYS_WRITE
    mov     x0, #1
    ldr     x1, =before_msg
    mov     x2, #before_len
    svc     #0

    mov     x8, #2          // SYS_SLEEP
    mov     x0, #1          // seconds = 1
    mov     x1, #0          // nanos = 0
    svc     #0

    mov     x8, #1          // SYS_WRITE
    mov     x0, #1
    ldr     x1, =after_msg
    mov     x2, #after_len
    svc     #0

    mov     x8, #7          // SYS_EXIT
    mov     x0, #0
    svc     #0
1:  b       1b
