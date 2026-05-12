bits 64
default rel

; ---------------------------------------------------------------
; Externs
; ---------------------------------------------------------------
extern syscall_dispatch
extern current_proc                ; struct proc * — set by exec() before ring-3 entry

; Byte offsets inside struct proc (must match proc.h)
%define PROC_KERNEL_RSP          0
%define PROC_USER_RSP            8
%define PROC_KERNEL_STACK_BASE  16
%define PROC_KERNEL_STACK_TOP   24

; ---------------------------------------------------------------
; Public symbols
; ---------------------------------------------------------------
global syscall_entry
global syscall_init
global syscall_scratch

; ---------------------------------------------------------------
; MSR addresses
; ---------------------------------------------------------------
%define MSR_EFER    0xC0000080
%define MSR_STAR    0xC0000081
%define MSR_LSTAR   0xC0000082
%define MSR_SFMASK  0xC0000084

; ---------------------------------------------------------------
; Userspace selectors (must match GDT and STAR[63:48] encoding)
;   GDT: 0x18 udata, 0x20 ucode. RPL = 3 -> OR with 3.
; ---------------------------------------------------------------
%define USER_CS     0x23
%define USER_SS     0x1B

; ---------------------------------------------------------------
; struct syscall_frame layout (must match proc.h)
; Lowest address first (i.e. last pushed):
;   r15, r14, r13, r12, r11, r10, r9, r8,
;   rbp, rdi, rsi, rdx, rcx, rbx, rax,
;   user_rip, user_rflags, user_rsp
; ---------------------------------------------------------------

section .data
syscall_scratch: dq 0

section .text

; ---------------------------------------------------------------
; void syscall_init(void)
;
; Sets the four MSRs that enable SYSCALL.  The kernel stack is NOT
; configured here — exec() populates current_proc before entering
; userspace.
;
; GDT layout assumed:
;   0x00 null | 0x08 kcode | 0x10 kdata | 0x18 udata | 0x20 ucode | 0x28 tss
;
; STAR encoding:
;   bits[47:32] = 0x0008  -> SYSCALL  CS = 0x08, SS = 0x10
;   bits[63:48] = 0x0010  -> SYSRET64 CS = 0x23, SS = 0x1B
;
; We return to userspace via IRETQ rather than SYSRETQ, but STAR[63:48]
; is still encoded in the standard way: the CPU validates it whenever
; SYSCALL is enabled.
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
    mov edx, 0x00100008          ; [63:48] = 0x0010, [47:32] = 0x0008
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
; syscall_entry — SYSCALL landing pad, IRETQ return path
;
; CPU state on arrival:
;   rax = syscall number
;   rdi = arg1, rsi = arg2, rdx = arg3
;   rcx = user return RIP   (saved by SYSCALL)
;   r11 = user RFLAGS       (saved by SYSCALL)
;   rsp = user stack        (not yet switched — we do it)
;   IF  = 0                 (cleared by SFMASK)
;
; We preserve the entire userspace register state except:
;   rax       = syscall return value
;   rcx / r11 = architecturally clobbered by SYSCALL (slots hold
;               user RIP and user RFLAGS, matching the SYSRET ABI;
;               user code must treat these as scratch).
; ---------------------------------------------------------------
syscall_entry:

    ; 1. Stash r10 so we can use it as a scratch register
    mov [syscall_scratch], r10
    mov r10, [current_proc]

    ; 2. Switch to the kernel stack
    mov [r10 + PROC_USER_RSP], rsp
    mov rsp, [r10 + PROC_KERNEL_STACK_TOP]

    ; 3. Build struct syscall_frame on the kernel stack.
    ;    First push -> highest offset, so the order below mirrors the
    ;    struct from last field down to first.
    push qword [r10 + PROC_USER_RSP]   ; user_rsp     @ offset 136
    push r11                           ; user_rflags  @ offset 128
    push rcx                           ; user_rip     @ offset 120

    push rax                           ; rax  @ 112
    push rbx                           ; rbx  @ 104
    push rcx                           ; rcx  @  96   (= user RIP per SYSCALL ABI)
    push rdx                           ; rdx  @  88
    push rsi                           ; rsi  @  80
    push rdi                           ; rdi  @  72
    push rbp                           ; rbp  @  64

    push r8                            ; r8   @  56
    push r9                            ; r9   @  48
    push qword [syscall_scratch]       ; r10  @  40   (original, pre-stash)
    push r11                           ; r11  @  32   (= user RFLAGS per SYSCALL ABI)
    push r12                           ; r12  @  24
    push r13                           ; r13  @  16
    push r14                           ; r14  @   8
    push r15                           ; r15  @   0   (last push, top of stack)

    ; 4. Call C dispatcher: uint64_t syscall_dispatch(struct syscall_frame *tf)
    mov rdi, rsp                       ; arg1: struct syscall_frame *
    mov rbp, rsp
    and rsp, ~0xF                      ; SysV stack alignment
    call syscall_dispatch
    mov rsp, rbp

    ; 5. Restore registers (rax slot is discarded — C return value already in rax)
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11                            ; user RFLAGS
    pop r10
    pop r9
    pop r8

    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx                            ; user RIP
    pop rbx

    add rsp, 8                         ; skip saved rax slot; keep C return value

    ; 6. Build the IRETQ frame from saved user_rip / user_rflags / user_rsp.
    ;
    ;   Stack currently:  [rsp+0]=user_rip  [rsp+8]=user_rflags  [rsp+16]=user_rsp
    ;   IRETQ expects:    [rsp+0]=RIP  [rsp+8]=CS  [rsp+16]=RFLAGS  [rsp+24]=RSP  [rsp+32]=SS
    ;
    ; Every GPR holds the user's restored state at this point and rax holds
    ; the syscall return value, so we stash rax in scratch memory, use it to
    ; shuffle the three saved fields into the iret layout, insert CS and SS,
    ; then reload rax. Growing rsp BEFORE writing keeps the construction
    ; NMI-safe (an NMI would push its own frame at the current rsp).
    mov [syscall_scratch], rax

    sub rsp, 16                        ; reserve slots for CS and SS

    mov rax, [rsp + 16]                ; user_rip  (was at old [rsp+0])
    mov [rsp + 0],  rax                ; -> RIP
    mov qword [rsp + 8],  USER_CS      ; -> CS
    mov rax, [rsp + 24]                ; user_rflags (was at old [rsp+8])
    mov [rsp + 16], rax                ; -> RFLAGS
    mov rax, [rsp + 32]                ; user_rsp  (was at old [rsp+16])
    mov [rsp + 24], rax                ; -> RSP
    mov qword [rsp + 32], USER_SS      ; -> SS

    mov rax, [syscall_scratch]         ; restore syscall return value

    ; 7. Return to userspace. IRETQ atomically loads RIP/CS/RFLAGS/RSP/SS
    ;    and performs the CPL change based on CS RPL.
    iretq