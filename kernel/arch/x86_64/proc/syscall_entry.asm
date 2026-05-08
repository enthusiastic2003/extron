bits 64
default rel

; ---------------------------------------------------------------
; Externs
; ---------------------------------------------------------------
extern syscall_dispatch
extern current_proc         ; struct proc * — set by exec() before ring-3 entry

; Byte offsets inside struct proc (must match proc.h)
%define PROC_KERNEL_RSP  0
%define PROC_USER_RSP    8

; ---------------------------------------------------------------
; Public symbols
; ---------------------------------------------------------------
global syscall_entry
global syscall_init

; ---------------------------------------------------------------
; MSR addresses
; ---------------------------------------------------------------
%define MSR_EFER    0xC0000080
%define MSR_STAR    0xC0000081
%define MSR_LSTAR   0xC0000082
%define MSR_SFMASK  0xC0000084

section .text

; ---------------------------------------------------------------
; void syscall_init(void)
;
; Sets the four MSRs that enable SYSCALL/SYSRET.
; The kernel stack is NOT set here — exec() populates current_proc
; before entering userspace.
;
; GDT layout assumed:
;   0x00 null | 0x08 kcode | 0x10 kdata | 0x18 udata | 0x20 ucode | 0x28 tss
;
; STAR encoding:
;   bits[47:32] = 0x0008  -> SYSCALL  CS=0x08, SS=0x10
;   bits[63:48] = 0x0010  -> SYSRET64 CS=(0x10+16)|3=0x23, SS=(0x10+8)|3=0x1B
; ---------------------------------------------------------------
syscall_init:
    ; Enable SYSCALL (SCE bit 0 in EFER)
    mov ecx, MSR_EFER
    rdmsr
    or  eax, 1
    wrmsr

    ; STAR — selector bases
    mov ecx, MSR_STAR
    xor eax, eax
    mov edx, 0x00100008     ; [63:48]=0x0010  [47:32]=0x0008
    wrmsr

    ; LSTAR — handler address
    mov ecx, MSR_LSTAR
    mov rax, syscall_entry
    mov rdx, rax
    shr rdx, 32
    wrmsr

    ; SFMASK — clear IF on SYSCALL entry
    mov ecx, MSR_SFMASK
    mov eax, (1 << 9)
    xor edx, edx
    wrmsr

    ret

; ---------------------------------------------------------------
; syscall_entry — SYSCALL landing pad
;
; CPU state on arrival:
;   rax = syscall number
;   rdi = arg1, rsi = arg2, rdx = arg3
;   rcx = user return RIP   (saved by SYSCALL)
;   r11 = user RFLAGS       (saved by SYSCALL)
;   rsp = user stack        (NOT switched — we do it)
;   IF  = 0                 (cleared by SFMASK)
; ---------------------------------------------------------------
syscall_entry:
    ; --- stack switch via current_proc ---
    ; r10 is caller-saved and not a syscall argument register for us.
    mov r10, [current_proc]         ; r10 = struct proc *
    mov [r10 + PROC_USER_RSP], rsp  ; proc->user_rsp  = user RSP
    mov rsp, [r10 + PROC_KERNEL_RSP]; RSP = proc->kernel_rsp

    ; --- save user return context + callee-saved regs ---
    push rcx        ; user RIP  (needed by SYSRET)
    push r11        ; user RFLAGS (needed by SYSRET)
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; --- re-order for C calling convention ---
    ; Want: rdi=nr, rsi=arg1, rdx=arg2, rcx=arg3
    ; Have: rax=nr, rdi=arg1, rsi=arg2, rdx=arg3
    ; Work back-to-front so no value is overwritten before it's moved.
    mov rcx, rdx    ; arg3 -> 4th C param  (rcx already saved on stack)
    mov rdx, rsi    ; arg2 -> 3rd C param
    mov rsi, rdi    ; arg1 -> 2nd C param
    mov rdi, rax    ; nr   -> 1st C param

    call syscall_dispatch
    ; return value now in rax

    ; --- restore callee-saved regs + return context ---
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop r11         ; user RFLAGS
    pop rcx         ; user RIP

    ; --- restore user RSP from proc struct (r10 is caller-saved) ---
    mov r10, [current_proc]
    mov [r10 + PROC_KERNEL_RSP], rsp    ; ← save kernel RSP back
    mov rsp, [r10 + PROC_USER_RSP]

    ; --- return to ring 3 ---
    o64 sysret