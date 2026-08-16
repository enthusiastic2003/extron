// Entry point for RPi4 (BCM2711, Cortex-A72, AArch64).
// QEMU's raspi4b machine (like real RPi4 firmware) starts every core here
// at 0x80000 in EL2, MMU off. We park cores 1-3 and let core 0 continue.
//
// Milestone 1 scope only: no MMU, no higher-half — everything runs
// physically identity-mapped-by-construction until Milestone 3.

.section ".text.boot"

.global _start
_start:
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, primary_core
hang:
    wfe
    b       hang

primary_core:
    ldr     x0, =_stack_top
    mov     sp, x0

    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
    sub     x1, x1, x0
    bl      clear_bss

    bl      kernel_aarch64_main
    b       hang

clear_bss:
    cbz     x1, clear_bss_done
    strb    wzr, [x0], #1
    sub     x1, x1, #1
    b       clear_bss
clear_bss_done:
    ret
