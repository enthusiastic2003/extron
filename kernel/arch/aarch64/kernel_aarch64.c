#include <kernel/drivers/serial.h>
#include <kernel/console.h>
#include <kernel/panic.h>
#include <kernel/arch.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
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

void arch_kernel_late_init(uint64_t mb2_addr) {
    init_paging(mb2_addr);
    pmm_print_stats();

    vmm_init();

    /* Proves vmm_alloc_pages/kmap work for real (not just init_paging's
     * own internal calls) — the same kind of concrete checkpoint used
     * for pmm_alloc_page() earlier, not just trusting the theory. */
    virt_addr_t stack_top = vmm_setup_stack();
    kprintf("aarch64: kernel stack via vmm_setup_stack() at %p\n", (void *)stack_top);

    kprintf("aarch64: Stage 1 complete. No Stage 2 yet (GDT/IDT/scheduler not implemented).\n");
    for (;;) {
        __asm__ volatile ("wfe");
    }
}
