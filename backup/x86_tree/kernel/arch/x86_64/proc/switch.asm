bits 64
default rel

; ---------------------------------------------------------------
; void context_switch(struct cpu_context *old, struct cpu_context *new);
;                      %rdi                     %rsi
;
; Saves all callee-saved registers + RIP + RSP into *old,
; then restores them from *new.
;
; struct cpu_context field offsets (must match proc.h):
;   r15  = 0x00    r14  = 0x08    r13  = 0x10
;   r12  = 0x18    rbx  = 0x20    rbp  = 0x28
;   rip  = 0x30    rsp  = 0x38
; ---------------------------------------------------------------

global context_switch

section .text

context_switch:
    ;--- Save callee-saved regs into old context struct ---
    mov [rdi + 0x00], r15
    mov [rdi + 0x08], r14
    mov [rdi + 0x10], r13
    mov [rdi + 0x18], r12
    mov [rdi + 0x20], rbx
    mov [rdi + 0x28], rbp

    ;--- Save RIP: store address of .resume label ---
    ;    When this process is switched back in, execution
    ;    resumes right after the context was saved.
    lea rax, [rel .resume]
    mov [rdi + 0x30], rax           ; old->rip = &.resume

    ;--- Save RSP ---
    mov [rdi + 0x38], rsp           ; old->rsp = current stack pointer

    ;--- Restore callee-saved regs from new context ---
    mov r15, [rsi + 0x00]
    mov r14, [rsi + 0x08]
    mov r13, [rsi + 0x10]
    mov r12, [rsi + 0x18]
    mov rbx, [rsi + 0x20]
    mov rbp, [rsi + 0x28]

    ;--- Restore RSP, then jump to saved RIP ---
    mov rsp, [rsi + 0x38]           ; new stack
    jmp [rsi + 0x30]                ; jump to new->rip

.resume:
    ; We land here when another process switches back to us.
    ; The C caller's stack frame is intact, so just return.
    ret
