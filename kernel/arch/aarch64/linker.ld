/*
 * Milestone 3, full pass: higher-half link matching x86's layout
 * (memory_map_decisions.md), bridged by an early physical-addressed
 * bootstrap in boot.S. RPi4 firmware always loads and jumps to physical
 * 0x80000 regardless of what VMA the linker assigns here — same trick
 * x86's linker.ld plays against GRUB's own fixed physical load address.
 */
OUTPUT_FORMAT(elf64-littleaarch64)
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_LMA = 0x80000;

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

    . = ALIGN(16);
    __bss_start = .;
    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA) {
        *(.bss .bss.*)
        *(COMMON)
    }
    __bss_end = .;

    . = ALIGN(16);
    . += 0x4000; /* 16KB boot stack */
    _stack_top = .;

    _kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.eh_frame_hdr*)
        *(.note*)
        *(.comment)
        *(.gcc_except_table*)
    }
}
