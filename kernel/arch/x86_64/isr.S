/*
 * x86-64 interrupt/exception/syscall entry. Every path builds the same
 * struct arch_frame (kernel/arch/x86_64/include/arch/exceptions.h) and
 * calls exception_dispatch(frame); every path returns with iretq, so
 * execve/sigreturn/fork-child only have to edit or fabricate a frame.
 */
.macro PUSH_REGS
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    mov  %cr2, %rax
    push %rax               /* cr2 */
    pushq $0                /* pad: 192-byte frame, 16-byte aligned */
.endm

.macro POP_REGS
    add  $16, %rsp          /* pad + cr2 */
    pop  %r15
    pop  %r14
    pop  %r13
    pop  %r12
    pop  %r11
    pop  %r10
    pop  %r9
    pop  %r8
    pop  %rbp
    pop  %rdi
    pop  %rsi
    pop  %rdx
    pop  %rcx
    pop  %rbx
    pop  %rax
.endm

.section .text

.macro ISR_NOERR n
.global isr_stub_\n
isr_stub_\n:
    pushq $0
    pushq $\n
    jmp  isr_common
.endm

.macro ISR_ERR n
.global isr_stub_\n
isr_stub_\n:
    pushq $\n
    jmp  isr_common
.endm

isr_common:
    PUSH_REGS
    cld
    mov  %rsp, %rdi
    call exception_dispatch
    POP_REGS
    add  $16, %rsp          /* vector + error code */
    iretq

.altmacro
.macro ISR_GEN n
  .if (\n == 8) || ((\n >= 10) && (\n <= 14)) || (\n == 17) || (\n == 21) || (\n == 29) || (\n == 30)
    ISR_ERR \n
  .else
    ISR_NOERR \n
  .endif
.endm
.macro ISR_TBL n
    .quad isr_stub_\n
.endm

.set i, 0
.rept 256
    ISR_GEN %i
    .set i, i+1
.endr

.section .rodata
.balign 8
.global isr_stub_table
isr_stub_table:
.set i, 0
.rept 256
    ISR_TBL %i
    .set i, i+1
.endr
.noaltmacro

.section .text

/* --- syscall entry (IA32_LSTAR) ---
 * On entry: rcx = user rip, r11 = user rflags, rsp = USER stack, IF
 * cleared by IA32_FMASK. There is no swapgs/per-CPU area (single core):
 * the user rsp is parked in a global for the two instructions it takes to
 * move onto the kernel stack. */
.global syscall_entry
syscall_entry:
    mov  %rsp, syscall_user_rsp(%rip)
    mov  kernel_rsp0(%rip), %rsp
    pushq $0x1B             /* ss */
    pushq syscall_user_rsp(%rip)
    push %r11               /* rflags */
    pushq $0x23             /* cs */
    push %rcx               /* rip */
    pushq $0                /* error code */
    pushq $0x80             /* vector */
    jmp  isr_common

/* --- where a freshly forked child starts ---
 * Reached by context_switch's `ret`, with rsp == the child's copy of the
 * parent's trap frame (proc_fork() arranged it). */
.global proc_fork_trampoline
proc_fork_trampoline:
    POP_REGS
    add  $16, %rsp
    iretq

/* void arch_enter_user(entry rdi, sp rsi, argc rdx, argv rcx) — noreturn */
.global arch_enter_user
arch_enter_user:
    cli
    pushq $0x1B             /* ss */
    push  %rsi              /* user rsp */
    pushq $0x202            /* rflags: IF */
    pushq $0x23             /* cs */
    push  %rdi              /* rip */
    mov   %rdx, %rdi        /* argc */
    mov   %rcx, %rsi        /* argv */
    xor   %eax, %eax
    xor   %ebx, %ebx
    xor   %ecx, %ecx
    xor   %edx, %edx
    xor   %ebp, %ebp
    xor   %r8d, %r8d
    xor   %r9d, %r9d
    xor   %r10d, %r10d
    xor   %r11d, %r11d
    xor   %r12d, %r12d
    xor   %r13d, %r13d
    xor   %r14d, %r14d
    xor   %r15d, %r15d
    iretq
