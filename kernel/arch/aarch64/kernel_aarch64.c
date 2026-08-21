#include <kernel/drivers/serial.h>
#include <kernel/panic.h>
#include "fdt.h"
#include "mb2_shim.h"

/*
 * Milestone 1/2 aarch64 entry point, called directly from boot.S with the
 * firmware-provided device tree pointer in x0. Converts that into a real
 * multiboot2 MMAP tag (see mb2_shim.c) and hands off to the SAME
 * kernel_stage1() the x86 side uses — see kernel/kernel.c for the shared,
 * compile-time arch-guarded init sequence. No C header declares
 * kernel_stage1 (the x86 side calls it from boot.asm via a bare `extern`
 * symbol too), so it's declared locally here the same way.
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
