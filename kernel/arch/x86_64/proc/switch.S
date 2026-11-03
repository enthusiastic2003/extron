// void context_switch(struct cpu_context *old /*rdi*/, struct cpu_context *new /*rsi*/);
//
// x86-64 counterpart of aarch64/proc/switch.S. The return address already
// sits on the stack (call pushed it), so saving rsp and later `ret`-ing off
// the new stack is the whole resume mechanism; first-launch contexts are
// fabricated with the entry point in the return slot (arch_context_init).
//
// Offsets (arch/context.h): rbx 0x00, rbp 0x08, r12 0x10, r13 0x18, r14 0x20,
// r15 0x28, rsp 0x30, fs_base 0x38, fxsave 0x40.
.section .text
.global context_switch
context_switch:
    mov  %rbx, 0x00(%rdi)
    mov  %rbp, 0x08(%rdi)
    mov  %r12, 0x10(%rdi)
    mov  %r13, 0x18(%rdi)
    mov  %r14, 0x20(%rdi)
    mov  %r15, 0x28(%rdi)
    mov  %rsp, 0x30(%rdi)

    mov  $0xC0000100, %ecx          // IA32_FS_BASE (user TLS pointer)
    rdmsr
    shl  $32, %rdx
    or   %rdx, %rax
    mov  %rax, 0x38(%rdi)
    fxsave 0x40(%rdi)

    mov  0x00(%rsi), %rbx
    mov  0x08(%rsi), %rbp
    mov  0x10(%rsi), %r12
    mov  0x18(%rsi), %r13
    mov  0x20(%rsi), %r14
    mov  0x28(%rsi), %r15

    fxrstor 0x40(%rsi)
    mov  $0xC0000100, %ecx
    mov  0x38(%rsi), %rax
    mov  %rax, %rdx
    shr  $32, %rdx
    wrmsr

    mov  0x30(%rsi), %rsp
    ret

.global cpu_context_save_fpsimd
cpu_context_save_fpsimd:
    mov  $0xC0000100, %ecx
    mov  %rdi, %r8
    rdmsr
    shl  $32, %rdx
    or   %rdx, %rax
    mov  %rax, 0x38(%r8)
    fxsave 0x40(%r8)
    ret

.global cpu_context_restore_fpsimd
cpu_context_restore_fpsimd:
    fxrstor 0x40(%rdi)
    mov  $0xC0000100, %ecx
    mov  0x38(%rdi), %rax
    mov  %rax, %rdx
    shr  $32, %rdx
    wrmsr
    ret
