/*
 * Stand-ins for the Raspberry Pi-only drivers (VideoCore mailbox +
 * framebuffer, BCM2711 power manager, Arasan EMMC) so shared kernel code
 * that references them links unchanged on x86-64. The root filesystem
 * comes from the UEFI loader's initrd module; the console is the COM1
 * serial port.
 */
#include <kernel/drivers/mailbox.h>
#include <kernel/drivers/power.h>
#include <kernel/drivers/emmc.h>
#include <kernel/console.h>
#include "io.h"

void mailbox_init(void) {}
void mailbox_report(void) {}
int mailbox_property_call(volatile uint32_t *msg) { (void)msg; return -1; }

void power_init(void) {}

void power_reset(void) {
    kprintf("power: resetting\n");
    /* Reset via the keyboard controller, then a triple fault as fallback. */
    outb(0x64, 0xFE);
    outb(0xCF9, 0x06);
    for (;;) __asm__ volatile ("cli; hlt");
}

void power_off(void) {
    /* QEMU/Bochs ACPI shutdown ports. */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    for (;;) __asm__ volatile ("cli; hlt");
}

void emmc_init(void) {}
void emmc_mount_ext2(void) { kprintf("emmc: not present on this platform\n"); }
int emmc_read_sectors(uint64_t lba, size_t count, void *buf) { (void)lba; (void)count; (void)buf; return -1; }
