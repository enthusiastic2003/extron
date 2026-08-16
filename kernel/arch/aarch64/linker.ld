/*
 * Milestone 1: physical, non-higher-half link at the RPi4 kernel8.img
 * load address. Revisit once Milestone 3 (MMU/paging) brings back the
 * higher-half layout from memory_map_decisions.md.
 */
OUTPUT_FORMAT(elf64-littleaarch64)
ENTRY(_start)

SECTIONS
{
    . = 0x80000;

    _kernel_start = .;

    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    . = ALIGN(16);
    __bss_start = .;
    .bss : {
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
