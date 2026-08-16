#include <kernel/drivers/serial.h>

/*
 * Milestone 1 entry point. Deliberately bypasses kernel/kernel.c and
 * kernel/console/console.c (both still x86/VGA-coupled) — this just
 * proves the boot -> UART path works under QEMU's raspi4b machine.
 * PMM, paging, interrupts, and everything else come in later milestones.
 */

static void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') {
            serial_putc('\r');
        }
        serial_putc(*s++);
    }
}

void kernel_aarch64_main(void) {
    init_serial();

    serial_puts("\n");
    serial_puts("=======================================\n");
    serial_puts(" Extron OS: AArch64 boot skeleton OK\n");
    serial_puts(" Running under QEMU raspi4b\n");
    serial_puts("=======================================\n");

    for (;;) {
        __asm__ volatile("wfe");
    }
}
