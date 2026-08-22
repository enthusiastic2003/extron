; ============================================================
; ISR stubs for all 256 interrupt vectors
;
; Vectors with CPU-pushed error codes:
;   8, 10, 11, 12, 13, 14, 17, 21, 29, 30
; All others get a dummy error code pushed (0).
; ============================================================

global isr_stub_table
extern isr_handler

section .text
bits 64

; ----- Macro: stub WITHOUT error code -----
%macro ISR_NO_ERR 1
isr_stub_%1:
    push 0              ; dummy error code
    push %1             ; vector number
    jmp isr_common
%endmacro

; ----- Macro: stub WITH error code (CPU already pushed it) -----
%macro ISR_ERR 1
isr_stub_%1:
    push %1             ; vector number (error code already on stack)
    jmp isr_common
%endmacro

; ============================================================
; Generate all 256 stubs
; ============================================================

; Exceptions 0-7: no error code
ISR_NO_ERR 0
ISR_NO_ERR 1
ISR_NO_ERR 2
ISR_NO_ERR 3
ISR_NO_ERR 4
ISR_NO_ERR 5
ISR_NO_ERR 6
ISR_NO_ERR 7

; Exception 8: Double Fault (has error code)
ISR_ERR 8

; Exception 9: no error code
ISR_NO_ERR 9

; Exceptions 10-14: have error code
ISR_ERR 10
ISR_ERR 11
ISR_ERR 12
ISR_ERR 13
ISR_ERR 14

; Exceptions 15-16: no error code
ISR_NO_ERR 15
ISR_NO_ERR 16

; Exception 17: Alignment Check (has error code)
ISR_ERR 17

; Exceptions 18-20: no error code
ISR_NO_ERR 18
ISR_NO_ERR 19
ISR_NO_ERR 20

; Exception 21: Control Protection (has error code)
ISR_ERR 21

; Exceptions 22-28: no error code
ISR_NO_ERR 22
ISR_NO_ERR 23
ISR_NO_ERR 24
ISR_NO_ERR 25
ISR_NO_ERR 26
ISR_NO_ERR 27
ISR_NO_ERR 28

; Exception 29: VMM Communication (has error code)
ISR_ERR 29

; Exception 30: Security Exception (has error code)
ISR_ERR 30

; Exception 31: reserved, no error code
ISR_NO_ERR 31

; IRQs and software interrupts 32-255: no error code
%assign i 32
%rep 224
    ISR_NO_ERR i
%assign i i+1
%endrep

; ============================================================
; Common handler — saves all registers, calls C isr_handler
; ============================================================
isr_common:
    ; save general-purpose registers
    push rax
    push rbx
    push rcx
    push rdx ; rdx
    push rsi ; rbp 
    push rdi ; rdi 
    push rbp ; rsi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp        ; arg1 = pointer to isr_frame
    call isr_handler

    ; restore general-purpose registers
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

    add rsp, 16         ; pop vector + error code
    iretq

; ============================================================
; Stub pointer table — used by idt_init() in C
; ============================================================
section .data
align 8
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_%+i
%assign i i+1
%endrep