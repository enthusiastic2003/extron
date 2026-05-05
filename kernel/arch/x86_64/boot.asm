global _start
extern kernel_stage1

; --- Constants ---
KERNEL_VMA equ 0xFFFFFFFF80000000

; --- Multiboot2 Header ---
MAGIC    equ 0xE85250D6
ARCH     equ 0
HDRLEN   equ (header_end - header_start)
CHECKSUM equ -(MAGIC + ARCH + HDRLEN)

section .multiboot2
align 8
header_start:
    dd MAGIC
    dd ARCH
    dd HDRLEN
    dd CHECKSUM
    dw 0, 0
    dd 8
header_end:

; --- Uninitialized Data (BSS) ---
section .bss
align 4096
p4_table:      resb 4096
p3_table:      resb 4096
p3_table_high: resb 4096     ; P3 for the higher-half map
p2_table:      resb 4096     ; The shared P2 (1 GiB)

align 16
boot_stack_bottom: resb 16384     ; 16 KB temporary stack
boot_stack_top:


align 8
mb2_ptr: resq 1              ; Storage for multiboot2 pointer

; --- 32-Bit Boot Entry ---
section .text
bits 32
_start:
    ; 1. Save Multiboot2 pointer (use physical address)
    mov [mb2_ptr - KERNEL_VMA], ebx

    ; 2. Setup the physical stack
    mov esp, boot_stack_top - KERNEL_VMA

    ; 3. Setup paging and transition to 64-bit
    call setup_paging
    call enable_long_mode

    ; 4. Load the 64-bit GDT (use physical address)
    lgdt [gdt64.pointer - KERNEL_VMA]

    ; 5. Jump to 64-bit code segment
    jmp gdt64.code:(long_mode_start - KERNEL_VMA)


; --- Paging Setup ---
setup_paging:
    ; (Remember to subtract KERNEL_VMA for every table address!)
    
    ; 1. The Identity Map (Temporary Scaffold)
    ; P4[0] -> P3
    mov eax, (p3_table - KERNEL_VMA)
    or  eax, 0b11 ; Present + Writable
    mov [(p4_table - KERNEL_VMA)], eax

    ; P3[0] -> P2
    mov eax, (p2_table - KERNEL_VMA)
    or  eax, 0b11
    mov [(p3_table - KERNEL_VMA)], eax

    ; 2. The Higher Half Map (The Bridge)
    ; P4[511] -> P3_HIGH (Offset 511 * 8 = 4088)
    mov eax, (p3_table_high - KERNEL_VMA)
    or  eax, 0b11
    mov [(p4_table - KERNEL_VMA) + 4088], eax

    ; P3_HIGH[510] -> P2 (Offset 510 * 8 = 4080)
    ; Points to the EXACT SAME p2_table!
    mov eax, (p2_table - KERNEL_VMA)
    or  eax, 0b11
    mov [(p3_table_high - KERNEL_VMA) + 4080], eax

    ; 3. Fill the shared P2 table with 512 Huge Pages (2 MiB each)
    mov ecx, 0
.map_p2:
    mov eax, 0x200000
    mul ecx
    or  eax, 0b10000011 ; Present + Writable + Huge Page
    mov [(p2_table - KERNEL_VMA) + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jne .map_p2

    ret


; --- CPU State Setup ---
enable_long_mode:
    ; Load CR3 with the physical address of P4
    mov eax, (p4_table - KERNEL_VMA)
    mov cr3, eax

    ; Enable Physical Address Extension (PAE)
    mov eax, cr4
    or  eax, (1 << 5)
    mov cr4, eax

    ; Enable Long Mode (LME) via EFER MSR
    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 8) | (1 << 11) ; LME is bit 8, NXE is bit 11
    wrmsr

    ; Enable Paging (PG) and Protected Mode (PE)
    mov eax, cr0
    or  eax, (1 << 31) | (1 << 0)
    mov cr0, eax

    ret


; --- 64-Bit Global Descriptor Table ---
section .rodata
global gdt64
align 8
gdt64:
    dq 0 ; Null descriptor
.code: equ $ - gdt64
    dq (1<<43)|(1<<44)|(1<<47)|(1<<53) ; Executable | Code | Present | 64-bit
.pointer:
    dw $ - gdt64 - 1
    dq gdt64 - KERNEL_VMA ; Physical address of the GDT!


; --- 64-Bit Kernel Entry ---
section .text
bits 64
long_mode_start:
    ; 1. Clear out the old 32-bit segment registers
    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; 2. THE STACK JUMP
    ; We are now in 64-bit mode with higher-half paging active!
    ; We move the stack pointer up to the virtual address.
    mov rsp, boot_stack_top

    ; 3. Pass Multiboot2 pointer (SysV ABI dictates first arg goes in RDI)
    ; We load it using the virtual address!
    mov rdi, [mb2_ptr]

    ; 4. THE LONG JUMP
    ; Force an absolute jump to the higher half C kernel
    mov rax, kernel_stage1
    call rax

    ; 5. Safety Net (We should never return here)
.hang:
    cli
    hlt
    jmp .hang