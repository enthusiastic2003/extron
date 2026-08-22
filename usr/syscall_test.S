// Test userland payload proving the real syscall round trip: issues a
// genuine svc with SYS_WRITE (x8=1, matching kernel/include/kernel/
// proc/syscall.h's numbering), then spins forever. If this string
// shows up on the console, it went through the real path — real EL0
// code, real svc trap, real syscall_dispatch(), real console_putc(),
// real return to EL0 — not a kprintf() from kernel code pretending.
.section ".rodata"
msg:
    .ascii "Hello from a real syscall!\n"
msg_len = . - msg

.section ".text"
.global _start
_start:
    mov     x8, #1          // SYS_WRITE
    mov     x0, #1          // fd = stdout
    ldr     x1, =msg        // buf
    mov     x2, #msg_len    // count
    svc     #0
1:  b       1b
