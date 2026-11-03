/*
 * x86-64 higher-half kernel. The UEFI loader (boot/uefi/loader.c) copies
 * each PT_LOAD segment to its physical address (VMA - KERNEL_VMA) and
 * jumps to _start there; boot.S builds the page tables that map the image
 * at KERNEL_VMA and enters C. Same layout trick the aarch64 build uses
 * against the RPi firmware's fixed load address.
 */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_LMA = 0x4000000;   /* 64 MiB: above OVMF's low-memory boot-services allocations (~24 MiB) */

SECTIONS
{
    . = KERNEL_VMA + KERNEL_LMA;

    _kernel_start = .;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data .data.*)
    }

    /* The boot stack lives inside the .bss output section so the ELF
     * segment's p_memsz (which the UEFI loader reserves) covers it. */
    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
        . = ALIGN(16);
        . += 0x4000; /* 16KB boot stack */
        _stack_top = .;
    }

    _kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.eh_frame_hdr*)
        *(.note*)
        *(.comment)
        *(.gcc_except_table*)
    }
}
