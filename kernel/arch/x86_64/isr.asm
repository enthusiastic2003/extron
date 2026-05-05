global isr0
global isr6
global isr8
global isr13
global isr14
global isr32
global isr33
global isr_common

extern isr_handler

section .text
bits 64

; =========================
; IRQs
; =========================
isr32:
    push 0      ; fake error code
    push 32     ; vector
    jmp isr_common

isr33:
    push 0      ; fake error code
    push 33     ; vector number
    jmp isr_common
; =========================
; NO ERROR CODE (fake it)
; =========================

isr0:
    push 0
    push 0
    jmp isr_common

isr6:
    push 0
    push 6
    jmp isr_common

; =========================
; HAS ERROR CODE (CPU pushes it)
; =========================

isr8:
    push 8
    jmp isr_common

isr13:
    push 13
    jmp isr_common

isr14:
    push 14
    jmp isr_common

; =========================
; COMMON HANDLER
; =========================

isr_common:
    ; save registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    call isr_handler

    ; restore registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16     ; pop vector + error code
    iretq