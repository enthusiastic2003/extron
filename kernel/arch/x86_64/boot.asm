global _start
extern kernel_main

; Multiboot2 header
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

section .bss
align 4096
p4_table: resb 4096          ; PML4
p3_table: resb 4096          ; PDPT
p2_table: resb 4096          ; PD (2 MiB pages)
align 16
stack_bottom: resb 16384
stack_top:

section .text
bits 32                      ; GRUB hands us 32-bit protected mode
_start:
    mov esp, stack_top

    call setup_paging
    call enable_long_mode

    ; load 64-bit GDT
    lgdt [gdt64.pointer]

    ; far jump to 64-bit code segment
    jmp gdt64.code:long_mode_start

; ---- paging: identity-map first 1 GiB with 2 MiB pages ----
setup_paging:
    ; P4[0] -> P3
    mov eax, p3_table
    or  eax, 0b11            ; present + writable
    mov [p4_table], eax

    ; P3[0] -> P2
    mov eax, p2_table
    or  eax, 0b11
    mov [p3_table], eax

    ; P2: 512 entries, each 2 MiB
    mov ecx, 0
.map_p2:
    mov eax, 0x200000        ; 2 MiB
    mul ecx
    or  eax, 0b10000011      ; present + writable + huge
    mov [p2_table + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jne .map_p2
    ret

enable_long_mode:
    ; load P4 into CR3
    mov eax, p4_table
    mov cr3, eax

    ; enable PAE (CR4.PAE)
    mov eax, cr4
    or  eax, (1 << 5)
    mov cr4, eax

    ; set EFER.LME
    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 8)
    wrmsr

    ; enable paging (CR0.PG) + protected mode
    mov eax, cr0
    or  eax, (1 << 31) | (1 << 0)
    mov cr0, eax
    ret

; ---- minimal 64-bit GDT ----
section .rodata
gdt64:
    dq 0                     ; null descriptor
.code: equ $ - gdt64
    dq (1<<43)|(1<<44)|(1<<47)|(1<<53)  ; code, 64-bit
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

section .text
bits 64
long_mode_start:
    ; zero segment registers
    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call kernel_main
.hang:
    cli
    hlt
    jmp .hang