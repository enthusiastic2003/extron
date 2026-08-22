// SYS_READ interactive proof (paired with usr/heartbeat_test.S). Blocks
// on a real SYS_READ for one byte, then SYS_WRITEs it straight back —
// if this only ever echoes once you actually type on the serial
// console, and the heartbeat proc's dots keep appearing in the
// meantime (kernel/drivers/keyboard.c's kbd_getc() -> sleep() really
// yielding the CPU, not busy-spinning), that's the round trip proven
// live: real keystroke -> UART ISR -> wakeup(&kbuf) -> scheduler
// resumes this proc -> SYS_READ returns -> echoed.
.section ".data"
cbuf:
    .byte 0

.section ".text"
.global _start
_start:
loop:
    mov     x8, #0          // SYS_READ
    mov     x0, #0          // fd = stdin
    ldr     x1, =cbuf
    mov     x2, #1          // count = 1
    svc     #0

    mov     x8, #1          // SYS_WRITE
    mov     x0, #1          // fd = stdout
    ldr     x1, =cbuf
    mov     x2, #1
    svc     #0

    b       loop
