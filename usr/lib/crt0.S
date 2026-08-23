// Userspace entry point for C payloads (usr/*.c). Assembly payloads
// (usr/*.S) define their own _start and never link this.
//
// The kernel enters here directly via eret with SP_EL0 already pointing
// at the top of the stack it mapped (kernel/proc/exec.c) and with argc
// and argv ALREADY IN x0/x1 — placed there by proc_bootstrap_trampoline()
// on first launch, or by sys_execve()'s frame rewrite. That is not the
// Linux ELF ABI, which puts argc at [sp] for the entry stub to unpack;
// it is the ordinary AAPCS64 argument convention, chosen because the
// kernel erets straight here and there is no dynamic loader in between
// that would need the stack layout.
//
// So there is genuinely nothing to unpack, and x0/x1 must be left
// exactly as they arrived. They used to be zeroed here, which was
// correct at the time — the kernel set neither, so main() would
// otherwise have received leftover register contents that looked just
// like a real argument vector.
.section ".text"
.global _start
_start:
    mov     x29, #0                 // terminate the frame-pointer chain so a
    mov     x30, #0                 // backtrace stops here instead of walking
                                    // into whatever the stack page held

    bl      main                    // x0 = argc, x1 = argv, straight from the kernel
    bl      exit                    // main's return value is already in w0
1:  b       1b                      // exit never returns; belt and braces
