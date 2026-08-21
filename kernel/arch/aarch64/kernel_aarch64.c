#include <kernel/drivers/serial.h>
#include <kernel/console.h>
#include <kernel/panic.h>
#include <kernel/arch.h>
#include <kernel/mm/pmm.h>
#include "fdt.h"
#include "mb2_shim.h"
#include "paging_aarch64.h"

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

    serial_puts("\n");
    serial_puts("=======================================\n");
    serial_puts(" Extron OS: AArch64 boot skeleton OK\n");
    serial_puts("=======================================\n");

    struct fdt_mem_region regions[8];
    size_t n = fdt_get_memory_regions((const void *)dtb_phys, regions, 8);

    static uint8_t mb2_shim_buf[512] __attribute__((aligned(8)));
    uint64_t mb2_addr = mb2_shim_build(regions, n, mb2_shim_buf, sizeof(mb2_shim_buf));
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
    pmm_print_stats();
    aarch64_paging_init(mb2_addr);

    kprintf("aarch64: Stage 1 complete. No Stage 2 yet (GDT/IDT/scheduler not implemented).\n");
    for (;;) {
        __asm__ volatile ("wfe");
    }
}
