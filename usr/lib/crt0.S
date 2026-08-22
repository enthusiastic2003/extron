// Userspace entry point for C payloads (usr/*.c). Assembly payloads
// (usr/*.S) define their own _start and never link this.
//
// The kernel enters here directly via eret with SP_EL0 already pointing
// at the top of the stack it mapped (proc_create_from_binary,
// kernel/proc/exec.c) — there is no argc/argv/envp on the stack and no
// dynamic loader, so there is nothing to unpack.
.section ".text"
.global _start
_start:
    mov     x29, #0                 // terminate the frame-pointer chain so a
    mov     x30, #0                 // backtrace stops here instead of walking
                                    // into whatever the stack page held

    // argc = 0, argv = NULL. There is no argv on the stack to unpack —
    // the kernel erets straight to _start — and leaving x0/x1 as whatever
    // the register file happened to hold would hand main() garbage that
    // looks exactly like a real argument vector.
    mov     x0, #0
    mov     x1, #0
    bl      main
    bl      exit                    // main's return value is already in w0
1:  b       1b                      // exit never returns; belt and braces
