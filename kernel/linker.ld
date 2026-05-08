OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

/* Define our higher-half virtual offset */
KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS {
    /* Set the Virtual Memory Address (VMA) to 1 MiB into the higher half */
    . = KERNEL_VMA + 1M;                     

    _kernel_start = .;           

    /* The AT() command sets the Load Memory Address (LMA) back to physical 1 MiB */
    .multiboot2 ALIGN(4K) : AT(ADDR(.multiboot2) - KERNEL_VMA) { 
        *(.multiboot2) 
    }

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) { 
        *(.text*)   
    }
    
    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) { 
        *(.rodata*) 
    }
    
    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) { 
        *(.data*)   
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA) {
        *(COMMON)
        *(.bss*)
    }

    /DISCARD/ : {
    *(.eh_frame*)
    *(.eh_frame_hdr*)
    *(.note*)
    *(.comment)
    *(.gcc_except_table*)
    }            

    _kernel_end = .;
}