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
global syscall_entry
extern current_proc
extern syscall_dispatch

; x86-64 SYSCALL ABI on entry:
;   rax = syscall number
;   rdi = arg1
;   rsi = arg2
;   rdx = arg3
;   rcx = user RIP
;   r11 = user RFLAGS
;
; We preserve the entire userspace register state except:
;   rax = syscall return value
;   rcx/r11 = architecturally clobbered by SYSCALL

syscall_entry:

    ; ------------------------------------------------------------
    ; Switch to kernel stack
    ; ------------------------------------------------------------

    mov r10, [current_proc]

    ; save user rsp
    mov [r10 + PROC_USER_RSP], rsp

    ; load kernel rsp
    mov rsp, [r10 + PROC_KERNEL_RSP]

    ; ------------------------------------------------------------
    ; Save full userspace context
    ; ------------------------------------------------------------

    ; syscall return state
    push rcx
    push r11

    ; caller-saved
    push rdi
    push rsi
    push rdx
    push r8
    push r9
    push r10

    ; callee-saved
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; ------------------------------------------------------------
    ; Prepare C calling convention
    ;
    ; syscall_dispatch(
    ;     nr,
    ;     arg1,
    ;     arg2,
    ;     arg3
    ; )
    ; ------------------------------------------------------------

    mov rcx, rdx
    mov rdx, rsi
    mov rsi, rdi
    mov rdi, rax

    ; stack is 16-byte aligned here
    call syscall_dispatch

    ; return value already in rax

    ; ------------------------------------------------------------
    ; Restore userspace registers
    ; ------------------------------------------------------------

    ; callee-saved
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ; caller-saved
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rsi
    pop rdi

    ; syscall return state
    pop r11
    pop rcx

    ; ------------------------------------------------------------
    ; Restore kernel stack pointer into proc struct
    ; ------------------------------------------------------------

    mov r10, [current_proc]
    mov [r10 + PROC_KERNEL_RSP], rsp

    ; fetch saved user rsp
    mov r10, [r10 + PROC_USER_RSP]

    ; ------------------------------------------------------------
    ; Build IRETQ frame
    ; ------------------------------------------------------------

    ; SS
    push 0x1B

    ; RSP
    push r10

    ; RFLAGS
    push r11

    ; CS
    push 0x23

    ; RIP
    push rcx

    ; avoid leaking kernel ptrs
    xor r10, r10

    iretq