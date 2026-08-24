#include <kernel/drivers/power.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/vmm.h> /* NEW_HDDM */
#include <kernel/console.h>
#include <kernel/panic.h>

/* BCM2711 (RPi4) peripheral base — same one uart.c and mailbox.c use.
 * Matches both real hardware and QEMU's raspi4b machine (whether QEMU's
 * model actually implements the watchdog-triggered reset is a separate
 * question from whether these registers exist at this address — see
 * power_reset()'s own comment). */
#define PERIPHERAL_PHYS 0xFE000000ULL
#define PM_PHYS         (PERIPHERAL_PHYS + 0x100000ULL)
#define PM_BASE         (NEW_HDDM + PM_PHYS)

#define PM_RSTC (PM_BASE + 0x1c)
#define PM_WDOG (PM_BASE + 0x24)

/* Required in the top byte of every PM register write, or the write is
 * silently ignored — the BCM SoC's one piece of write protection against
 * a stray pointer bug resetting the board by accident. */
#define PM_PASSWORD 0x5A000000U

#define PM_RSTC_WRCFG_FULL_RESET 0x00000020U

static inline void mmio_write(unsigned long addr, unsigned int val) {
    *(volatile unsigned int *)addr = val;
}
static inline unsigned int mmio_read(unsigned long addr) {
    return *(volatile unsigned int *)addr;
}

/* Same self-mapping convention every other MMIO-touching driver uses
 * (mailbox.c's mailbox_init(), gic.c) — kmap() into the kernel's own
 * high-half table, Device memory, kernel-only (no PAGE_USER). Fits in
 * one page: PM_PHYS is already page-aligned and both registers this
 * driver touches sit in the first 0x28 bytes of it. */
void power_init(void) {
    if (kmap(PM_BASE, PM_PHYS,
             PAGE_PRESENT | PAGE_WRITE | PAGE_NX | PAGE_CACHE_DISABLE) != 0)
        panic("power: failed to map PM MMIO at %p", (void *)PM_PHYS);
}

void power_reset(void) {
    /* Arm the watchdog with the shortest useful countdown (PM_WDOG's
     * units are roughly 1/16000s) — this isn't "reboot in N seconds",
     * it's "reboot as soon as physically possible" with a nonzero timer
     * because 0 itself has no defined countdown behavior. */
    mmio_write(PM_WDOG, PM_PASSWORD | 1);
    unsigned int rstc = mmio_read(PM_RSTC);
    mmio_write(PM_RSTC, PM_PASSWORD | (rstc & ~0xFFFFFFCFU)
                                    | PM_RSTC_WRCFG_FULL_RESET);

    /* The watchdog fires within microseconds on real hardware. If it
     * doesn't (a QEMU machine model that accepts the write but doesn't
     * act on it), looping here is still correct: the syscall must never
     * return to a caller that's mid-reboot. */
    kprintf("power: watchdog armed, waiting for reset...\n");
    for (;;) {
        __asm__ volatile ("wfe");
    }
}
