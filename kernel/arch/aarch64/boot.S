// Entry point for RPi4 (BCM2711, Cortex-A72, AArch64).
// QEMU's raspi4b machine (like real RPi4 firmware) starts every core here
// at 0x80000 in EL2, MMU off. We park cores 1-3 and let core 0 continue.
//
// Milestone 1 scope only: no MMU, no higher-half — everything runs
// physically identity-mapped-by-construction until Milestone 3.

.section ".text.boot"

.global _start
_start:
    // Firmware passes the physical address of the device tree blob in x0
    // (standard arm64 boot protocol). Stash it in a callee-saved register
    // before anything else touches x0.
    mov     x19, x0

    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, primary_core
hang:
    wfe
    b       hang

primary_core:
    // Ensure our freshly-loaded code is actually coherent before we
    // execute any of it. Firmware doesn't guarantee this, and a warm
    // reset (re-flashing the SD card without a full power cycle) can
    // leave stale cache state from a PREVIOUS kernel run at these same
    // physical addresses. Full textbook sequence (same as Linux's own
    // head.S): clean D-cache to point of unification by VA over the
    // whole image first — I-cache invalidation alone isn't enough if any
    // dirty D-cache lines for our code were never written back to RAM,
    // since instruction fetch reads straight from RAM, bypassing D-cache.
    mrs     x2, ctr_el0
    ubfx    x2, x2, #16, #4     // CTR_EL0.DminLine
    mov     x3, #4
    lsl     x2, x3, x2          // x2 = D-cache line size in bytes

    ldr     x0, =_kernel_start
    ldr     x1, =_kernel_end
clean_dcache_loop:
    dc      cvau, x0
    add     x0, x0, x2
    cmp     x0, x1
    b.lo    clean_dcache_loop

    dsb     ish

    ic      ialluis
    dsb     ish
    isb

    // Firmware starts every core at EL2. If we're still there, drop to
    // EL1 before doing anything else — standard for any non-hypervisor
    // ARM64 kernel (Linux's own head.S does the same thing). This matters
    // architecturally, not just stylistically: at EL2 (non-VHE), the MMU/
    // cache-enable state governing OUR OWN currently-executing code comes
    // from SCTLR_EL2/TTBR0_EL2/TCR_EL2/MAIR_EL2, not the EL1 versions of
    // those registers. Setting SCTLR_EL1.M later (paging_aarch64.c) would
    // silently do nothing for code that's still actually running at EL2.
    mrs     x0, CurrentEL
    and     x0, x0, #0xC        // bits [3:2]
    cmp     x0, #0x8            // EL2 == 0b10 << 2
    b.ne    already_el1

    // HCR_EL2.RW=1: EL1 executes in AArch64 (not AArch32 — critical, or
    // EL1 defaults to the wrong execution state entirely).
    mov     x0, #1
    lsl     x0, x0, #31
    msr     hcr_el2, x0

    // Let EL1 access the physical generic timer without trapping to EL2
    // (needed later, harmless now).
    mov     x0, #3
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr

    // Target EL1h (SP_EL1), all exception classes masked — no handlers
    // exist yet (Milestone 4).
    mov     x0, #0x3c5
    msr     spsr_el2, x0

    adr     x0, already_el1
    msr     elr_el2, x0
    eret

already_el1:
    // CPACR_EL1.FPEN traps ALL FP/SIMD access at EL1 by default (reset
    // value), and nothing has configured it — firmware only ever ran at
    // EL2, never EL1. GCC uses FP/SIMD registers for things that have
    // nothing to do with floating point (e.g. __builtin_popcount on
    // aarch64 compiles to fmov/cnt/addv; variadic-function prologues
    // like kprintf's conventionally touch the FP register save area too,
    // per AAPCS64), so this bites almost immediately, not just for code
    // that looks like it uses floats. FPEN=0b11: no trapping at EL0/EL1.
    mov     x0, #3
    lsl     x0, x0, #20
    msr     cpacr_el1, x0
    isb

    ldr     x0, =_stack_top
    mov     sp, x0

    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
    sub     x1, x1, x0
    bl      clear_bss

    mov     x0, x19
    bl      kernel_aarch64_main
    b       hang

clear_bss:
    cbz     x1, clear_bss_done
    strb    wzr, [x0], #1
    sub     x1, x1, #1
    b       clear_bss
clear_bss_done:
    ret
