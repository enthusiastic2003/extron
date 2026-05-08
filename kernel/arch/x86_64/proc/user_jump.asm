; void enter_userspace(uint64_t entry, uint64_t user_rsp);
;                       %rdi              %rsi
global enter_userspace
enter_userspace:
    mov ax, 0x1B           ; user DATA selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push 0x1B              ; SS  = user data
    push rsi               ; RSP = user stack
    push 0x202             ; RFLAGS (IF=1, bit 1 reserved=1)
    push 0x23              ; CS  = user code
    push rdi               ; RIP = entry point
    iretq