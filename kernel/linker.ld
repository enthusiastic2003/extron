OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS {
    . = 1M;                      /* load at 1 MiB */

    .multiboot2 : { *(.multiboot2) }

    .text   : { *(.text*)   }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*)   }

    .bss : {
        *(COMMON)
        *(.bss*)
    }
}