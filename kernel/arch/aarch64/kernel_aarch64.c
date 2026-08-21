#include <kernel/drivers/serial.h>
#include <kernel/console.h>
#include <kernel/panic.h>
#include <kernel/arch.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
#include <arch/exceptions.h>
#include "fdt.h"
#include "mb2_shim.h"

/*
 * Milestone 1/2/3 aarch64 entry point, called directly from boot.S with
 * the firmware-provided device tree pointer in x0. Converts that into a
 * real multiboot2 MMAP tag (see mb2_shim.c) and hands off to the SAME
 * kernel_stage1() the x86 side uses — see kernel/kernel.c for the shared
 * entry point, and kernel/include/kernel/arch.h for the per-arch contract
 * this file implements (the aarch64 counterpart to
 * kernel/arch/x86_64/kernel_x86.c). No C header declares kernel_stage1
 * itself (the x86 side calls it from boot.asm via a bare `extern` symbol
 * too), so it's declared locally here the same way.
 */
extern void kernel_stage1(uint64_t mb2_addr);

static void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') {
            serial_putc('\r');
        }
        serial_putc(*s++);
    }
}

void kernel_aarch64_main(uint64_t dtb_phys) {
    init_serial();

    {
        uint64_t current_el;
        __asm__ volatile ("mrs %0, CurrentEL" : "=r"(current_el));
        unsigned el = (unsigned)((current_el >> 2) & 0x3);
        serial_puts("Current EL: ");
        serial_putc((char)('0' + el));
        serial_puts("\n");
    }

    serial_puts("\n");
    serial_puts("=======================================\n");
    serial_puts(" Extron OS: AArch64 boot skeleton OK\n");
    serial_puts("=======================================\n");

    struct fdt_mem_region regions[8];
    size_t n = fdt_get_memory_regions((const void *)dtb_phys, regions, 8);

    /* A `static` buffer would land in .bss, which is now high-VMA linked
     * like everything else in the higher-half build — but init_pmm()'s
     * ONE_GIB sanity check assumes mb2_addr is a genuinely low,
     * GRUB-style physical pointer (true on x86; not true of our own
     * high-VMA .bss). Rather than touch that shared x86-oriented check,
     * keep the shim buffer at a fixed low physical scratch address —
     * well below our kernel's tiny footprint's ceiling, comfortably
     * inside the 1GB identity map boot.S's early bootstrap sets up. */
    void *mb2_shim_buf = (void *)0x100000;
    uint64_t mb2_addr = mb2_shim_build(regions, n, mb2_shim_buf, 512);
    if (mb2_addr == 0) {
        panic("mb2_shim_build failed (buffer too small for %u regions)", (unsigned)n);
    }

    kernel_stage1(mb2_addr);

    // kernel_stage1 never returns on either arch, but just in case:
    for (;;) {
        __asm__ volatile("wfe");
    }
}

/* --- kernel/include/kernel/arch.h contract --- */

void arch_disable_interrupts(void) {
    /* No interrupts enabled yet on aarch64 (Milestone 4: GIC/exceptions),
     * so there's nothing to mask. */
}

void arch_halt_forever(void) {
    for (;;) {
        __asm__ volatile ("wfe");
    }
}

void arch_update_hw_cursor(int x, int y) {
    /* No VGA text-mode hardware cursor outside x86; kprintf's real output
     * on aarch64 is the serial_putc() call in console.c's putc(). */
    (void)x;
    (void)y;
}

void arch_kernel_early_init(void) {
    /* Nothing yet — no IDT/PIC equivalent until Milestone 4. */
}

void arch_kernel_mid_init(void) {
    /* Nothing yet — no GDT equivalent on aarch64. */
}

/**
 * @brief aarch64 Stage 2: runs on the permanent VMM-allocated kernel
 * stack instead of boot.S's temporary one. The aarch64 counterpart to
 * kernel_stage2() in kernel/arch/x86_64/kernel_x86.c — reached the same
 * way, via a raw stack-pointer switch + branch, never returning to
 * kernel_stage1. There's nothing to actually do here yet (GDT/IDT/GIC/
 * scheduler don't exist — Milestone 4+), so it's just proof the switch
 * itself works. mb2_addr is unused for now (x86's kernel_stage2 needs
 * it for tar_init(); aarch64 has no initrd yet).
 */
static void kernel_aarch64_stage2(uint64_t mb2_addr) {
    (void)mb2_addr;
    kprintf("--- AArch64 Kernel Stage 2 ---\n");
    kprintf("Successfully running on the VMM-allocated kernel stack.\n");

    exceptions_init();
    kprintf("aarch64: VBAR_EL1 set, IRQs unmasked.\n");

    /* Milestone 4, first proof point: deliberately trigger a
     * synchronous exception (SVC — clean and side-effect-free, unlike
     * e.g. a null-pointer deref) and confirm the vector table actually
     * catches it (via exception_dispatch's panic) instead of hanging
     * silently, before building GIC/IRQ handling on top of this. */
    kprintf("aarch64: triggering a test SVC to prove the vector table works...\n");
    __asm__ volatile ("svc #0");

    // Only reached if exception_dispatch somehow returned (it shouldn't — panic() halts).
    for (;;) {
        __asm__ volatile ("wfe");
    }
}

void arch_kernel_jump_to_stage2(uint64_t mb2_addr, uint64_t new_stack_top) {
    /* Perform the stack switch and jump to Stage 2 — same trick x86's
     * kernel_x86.c uses: move sp to the new stack, then branch (not
     * call — we never return here, so there's nothing to return to and
     * no reason to link). mb2_addr needs to land in x0 per AAPCS64
     * (matching how x86 passes it in rdi via the "D" constraint) — a
     * local register variable pins it there directly, rather than an
     * extra `mov x0, ...` inside the asm that could collide with
     * whatever register the branch-target operand happens to get
     * allocated (x86's "D" constraint sidesteps the same hazard). */
    register uint64_t arg0 __asm__("x0") = mb2_addr;
    __asm__ volatile (
        "mov sp, %1\n\t"
        "br  %2"
        :
        : "r"(arg0), "r"(new_stack_top), "r"(kernel_aarch64_stage2)
        : "memory"
    );

    // We should never reach here
    for (;;) {
        __asm__ volatile ("wfe");
    }
}
