ENTRY(_start)
SECTIONS {
    . = 0x400000;

    __ehdr_start = .;

    .text : {
        *(.text*)
    }
    .rodata : {
        *(.rodata*)
    }
    .data.rel.ro : { *(.data.rel.ro*) }
    .got         : { *(.got .got.*) }

    .preinit_array : {
        __preinit_array_start = .;
        KEEP(*(.preinit_array))
        __preinit_array_end = .;
    }
    .init_array : {
        __init_array_start = .;
        KEEP(*(.init_array*))
        __init_array_end = .;
    }
    .fini_array : {
        __fini_array_start = .;
        KEEP(*(.fini_array*))
        __fini_array_end = .;
    }

    .ctors : {
        __CTOR_LIST__ = .;
        KEEP(*(.ctors))
        __CTOR_END__ = .;
    }
    .dtors : {
        __DTOR_LIST__ = .;
        KEEP(*(.dtors))
        __DTOR_END__ = .;
    }

    .data : {
        __dso_handle = .;
        *(.data*)
    }
    .bss : {
        *(.bss*)
        *(COMMON)
    }

    /DISCARD/ : { *(.eh_frame*) *(.note*) }
}