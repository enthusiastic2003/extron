// Entry point for RPi4 (BCM2711, Cortex-A72, AArch64).
// Milestone 3, full pass: higher-half kernel. RPi4 firmware (like QEMU's
// raspi4b machine) always loads and jumps to physical 0x80000, MMU off,
// regardless of what VMA the linker assigns (see linker.ld) — same trick
// x86's boot.asm plays against GRUB's own fixed physical load address.
//
// This file bridges that gap: build a minimal set of early page tables
// (temporary scaffolding only — replaced wholesale once C code takes
// over in paging_aarch64.c), enable the MMU, then do an absolute jump to
// the high-half continuation. PC-relative branches (b/bl/adr) stay valid
// throughout Phase A even though we're executing at the physical (not
// linked) address, since the LMA/VMA offset is constant across the whole
// image and cancels out in any relative displacement — only absolute
// address LOADS (ldr xN, =symbol) need explicit "- KERNEL_VMA" until
// after the jump.
//
// Verified end-to-end on both QEMU raspi4b and real RPi4 hardware:
// reaches high-half C execution with the MMU + caches on, DTB and UART
// still reachable.

.equ KERNEL_VMA, 0xFFFFFFFF80000000

.section ".text.boot"

// _start MUST be the literal first thing in this section: kernel8.img is
// a flat binary (objcopy -O binary), and QEMU/real RPi4 firmware always
// jump to the physical load address's very first byte, completely
// ignoring the ELF's ENTRY(_start) metadata (that only matters to an
// actual ELF loader, which this boot path isn't). Anything placed before
// this label becomes the real accidental entry point instead.
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
    // === Phase A: executing at the LOW physical address (MMU off). ===

    // Ensure our freshly-loaded code is actually coherent before we
    // execute any of it (firmware doesn't guarantee this, and a warm
    // reset can leave stale cache state from a PREVIOUS kernel run).
    mrs     x2, ctr_el0
    ubfx    x2, x2, #16, #4     // CTR_EL0.DminLine
    mov     x3, #4
    lsl     x2, x3, x2          // x2 = D-cache line size in bytes

    ldr     x0, =(_kernel_start - KERNEL_VMA)
    ldr     x1, =(_kernel_end - KERNEL_VMA)
clean_dcache_loop:
    dc      cvau, x0
    add     x0, x0, x2
    cmp     x0, x1
    b.lo    clean_dcache_loop

    dsb     ish
    ic      ialluis
    dsb     ish
    isb

    // Firmware starts every core at EL2. Drop to EL1 before doing
    // anything else — standard for a non-hypervisor kernel; matters
    // architecturally since at EL2 (non-VHE) the MMU/cache state
    // governing OUR OWN code comes from the EL2 register set, not EL1.
    mrs     x0, CurrentEL
    and     x0, x0, #0xC
    cmp     x0, #0x8
    b.ne    skip_el2_setup

    // HCR_EL2.RW=1: EL1 executes in AArch64.
    mov     x0, #1
    lsl     x0, x0, #31
    msr     hcr_el2, x0

    // Let EL1 access the physical generic timer without trapping later.
    mov     x0, #3
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr

    // Target EL1h, all exception classes masked — no handlers exist yet.
    mov     x0, #0x3c5
    msr     spsr_el2, x0

    adr     x0, skip_el2_setup
    msr     elr_el2, x0
    eret

skip_el2_setup:
    // CPACR_EL1: allow FP/SIMD at EL1 — traps by default, and GCC uses
    // FP/SIMD registers for things unrelated to floating point
    // (__builtin_popcount, variadic-function prologues per AAPCS64).
    mov     x0, #3
    lsl     x0, x0, #20
    msr     cpacr_el1, x0
    isb

    // Clear ALL of .bss now, physically, BEFORE building the early page
    // tables below — they live in .bss too, and clearing it AFTER the
    // high-half jump (like Milestone 1/2 did) would zero out the very
    // page tables we're actively executing under at that point.
    ldr     x0, =(__bss_start - KERNEL_VMA)
    ldr     x1, =(__bss_end - KERNEL_VMA)
    sub     x1, x1, x0
    bl      clear_bss

    // --- Minimal early page tables: temporary scaffolding just to
    // survive turning the MMU on and reach the high-half continuation.
    // Identity-mapped under TTBR0 (so Phase A's own physical PC keeps
    // fetching correctly, and Phase B's C code can still read the
    // firmware-provided DTB and UART by their raw physical addresses)
    // and ALSO mapped at KERNEL_VMA under TTBR1 (so the jump target is
    // valid). Same trick x86's boot.asm uses (shared p2_table between
    // P3_table/P3_table_high). Index math (L0=511, L1=510 for the high
    // side) verified against KERNEL_VMA.

    // Shared L2 table: entry[0] = 2MB block, PA 0x0, Normal/RWX/AF/SH.
    // Used by the TTBR1/high side below (kernel's own tiny footprint).
    ldr     x0, =(early_l2_shared - KERNEL_VMA)
    mov     x1, #0x701          // VALID | AF | SH(inner) | AttrIndx(0=Normal)
    str     x1, [x0]

    // TTBR0 side: l0[0] -> l1_ttbr0. l1_ttbr0[0] is a DIRECT 1GB BLOCK
    // (not a table -> L2), identity-mapping PA[0, 0x40000000) — same
    // 1GB x86's own boot.asm identity-maps. Needs to be this large, not
    // just our own tiny kernel footprint: Phase B's C code
    // (fdt_get_memory_regions) must also read the firmware-provided
    // DTB, whose physical address isn't known until runtime and can sit
    // anywhere in the first ~1GB on both QEMU and real RPi4 hardware.
    ldr     x0, =(early_l1_ttbr0 - KERNEL_VMA)
    mov     x1, #0x701          // VALID | AF | SH(inner) | AttrIndx(0=Normal)
    str     x1, [x0]            // l1_ttbr0[0] = 1GB block, PA 0x0

    ldr     x0, =(early_l0_ttbr0 - KERNEL_VMA)
    ldr     x1, =(early_l1_ttbr0 - KERNEL_VMA)
    orr     x1, x1, #0x3
    str     x1, [x0]

    // UART page (0xFE201000, 2MB-aligned block base 0xFE200000, L1[3]/
    // L2[497]) — Device memory, still identity-addressed under TTBR0.
    // Without this, the instant the MMU turns on, uart.c's real
    // init_serial/serial_putc (which use this raw physical constant
    // unconditionally) fault trying to reach unmapped UART registers —
    // silent hang, no vector to catch it.
    ldr     x0, =(early_l2_uart - KERNEL_VMA)
    ldr     x1, =0xFE200000
    mov     x2, #0x405          // VALID | AF | AttrIndx(1=Device)
    orr     x1, x1, x2
    str     x1, [x0, #(497 * 8)]

    ldr     x0, =(early_l1_ttbr0 - KERNEL_VMA)
    ldr     x1, =(early_l2_uart - KERNEL_VMA)
    orr     x1, x1, #0x3
    str     x1, [x0, #(3 * 8)]

    // TTBR1 side: l0[511] -> l1_ttbr1, l1_ttbr1[510] -> l2_shared.
    ldr     x0, =(early_l1_ttbr1 - KERNEL_VMA)
    ldr     x1, =(early_l2_shared - KERNEL_VMA)
    orr     x1, x1, #0x3
    str     x1, [x0, #(510 * 8)]

    ldr     x0, =(early_l0_ttbr1 - KERNEL_VMA)
    ldr     x1, =(early_l1_ttbr1 - KERNEL_VMA)
    orr     x1, x1, #0x3
    str     x1, [x0, #(511 * 8)]

    // --- MAIR_EL1: index 0 = Normal WB/WA cacheable, index 1 = Device ---
    mov     x0, #0xFF
    msr     mair_el1, x0

    // --- TCR_EL1: 48-bit VA both halves, 4KB granule, TTBR1 enabled
    // (unlike the Milestone 3 minimal pass, which disabled it via EPD1).
    // T0SZ=16 | IRGN0=WBWA | ORGN0=WBWA | SH0=inner | TG0=4K
    // | T1SZ=16 | IRGN1=WBWA | ORGN1=WBWA | SH1=inner | TG1=4K | IPS=40bit
    ldr     x0, =0x2B5103510
    msr     tcr_el1, x0

    // --- TTBR0_EL1 / TTBR1_EL1 ---
    ldr     x0, =(early_l0_ttbr0 - KERNEL_VMA)
    msr     ttbr0_el1, x0
    ldr     x0, =(early_l0_ttbr1 - KERNEL_VMA)
    msr     ttbr1_el1, x0

    isb

    // --- enable MMU + caches (SCTLR_EL1.M/C/I). Read-modify-write:
    // SCTLR_EL1 has RES1 bits we must not clear. Caches must be on
    // (not just the MMU) — see paging_aarch64.c's comment on why the
    // exclusive-monitor mechanism spin_lock relies on needs this. ---
    mrs     x0, sctlr_el1
    orr     x0, x0, #1              // M
    orr     x0, x0, #(1 << 2)       // C
    orr     x0, x0, #(1 << 12)      // I
    msr     sctlr_el1, x0
    isb

    // === Bridge to Phase B: absolute jump to the high-half
    // continuation. Its address, as embedded here by the linker, IS the
    // high VMA — a PC-relative branch can't reach this far. ===
    ldr     x0, =high_half_entry
    br      x0

.section ".bss"
.balign 4096
early_l0_ttbr0:  .space 4096
early_l1_ttbr0:  .space 4096
early_l0_ttbr1:  .space 4096
early_l1_ttbr1:  .space 4096
early_l2_shared: .space 4096
early_l2_uart:   .space 4096

.section ".text"
high_half_entry:
    // === Phase B: now executing at the high VMA. Normal from here on.
    // .bss is already cleared (done in Phase A, physically, before the
    // early page tables — see comment above), so no clear_bss call here.
    ldr     x0, =_stack_top
    mov     sp, x0

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
