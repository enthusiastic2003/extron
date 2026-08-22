// Background heartbeat for the SYS_READ interactive proof (paired with
// usr/read_echo_test.S) — prints "." every ~200ms via a real SYS_SLEEP.
// If these dots keep landing on the console while the reader proc is
// blocked waiting for a keystroke, that's direct evidence the reader
// really gave up the CPU (kbd_getc() -> sleep()) instead of hogging it
// in a busy loop.
.section ".rodata"
dot_msg:
    .ascii "."
dot_len = . - dot_msg

.section ".text"
.global _start
_start:
loop:
    mov     x8, #1          // SYS_WRITE
    mov     x0, #1
    ldr     x1, =dot_msg
    mov     x2, #dot_len
    svc     #0

    mov     x8, #2          // SYS_SLEEP
    mov     x0, #0          // seconds = 0
    movz    x1, #0xC200, lsl #0    // nanos = 200,000,000
    movk    x1, #0x0BEB, lsl #16
    svc     #0

    b       loop
