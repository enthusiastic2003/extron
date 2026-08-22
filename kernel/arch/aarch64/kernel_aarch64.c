#include <kernel/drivers/serial.h>
#include <kernel/console.h>
#include <kernel/panic.h>
#include <kernel/arch.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
#include <arch/exceptions.h>
#include <arch/gic.h>
#include <kernel/drivers/timer.h>
#include <arch/vma.h>
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

/* kernel/kernel.c — arch-neutral continuation, called once this file's
 * own Stage 2 finishes genuinely arch-specific hardware bring-up
 * (interrupt controller, timer, exceptions unmasked). Same "no header,
 * bare extern" convention as kernel_stage1 above. */
extern void kernel_stage2(uint64_t mb2_addr) __attribute__((noreturn));

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

    /* RPi4 firmware's config.txt "initramfs" directive loads a file into
     * RAM and patches the runtime DTB's /chosen node with its physical
     * bounds — the same linux,initrd-start/-end convention real Linux
     * relies on. Absent entirely if config.txt has no "initramfs" line
     * (fdt_get_initrd_region returns 0), in which case mb2_shim_build()
     * below just omits the MODULE tag and tar_init() finds nothing, same
     * as x86 booting without a GRUB module. */
    uint64_t initrd_start = 0, initrd_end = 0;
    fdt_get_initrd_region((const void *)dtb_phys, &initrd_start, &initrd_end);

    /* Keep the DTB out of the physical allocator.
     *
     * Nothing reads it after this function — the memory regions and
     * initrd bounds above are copied into locals, and dtb_phys is never
     * touched again — so today the pages would be safe to recycle. This
     * reserves them anyway, because that safety is a property of what
     * the kernel currently happens to do rather than anything enforced,
     * and the first feature that wants to re-walk the device tree
     * (probing the VideoCore mailbox from /soc rather than hardcoding
     * its address, say) would silently be parsing recycled memory. A
     * DTB is tens of KB; the reservation costs nothing worth counting.
     *
     * This is what /memreserve/ exists to express: RAM that is inside a
     * usable range but occupied, as opposed to a hole in /memory, which
     * says the range is not usable RAM at all. */
    uint32_t dtb_size = fdt_get_total_size((const void *)dtb_phys);
    pmm_reserve_boot_region(dtb_phys, dtb_size);

    /* A `static` buffer would land in .bss, which is now high-VMA linked
     * like everything else in the higher-half build — but init_pmm()'s
     * ONE_GIB sanity check assumes mb2_addr is a genuinely low,
     * GRUB-style physical pointer (true on x86; not true of our own
     * high-VMA .bss). Rather than a hardcoded low physical scratch
     * address (which would need manually bumping if the kernel image or
     * the initrd ever grew enough to reach it — a real fragility, not a
     * hypothetical one, since both addresses come from independent
     * sources: KERNEL_LMA is fixed at link time, and RPi4 firmware picks
     * wherever config.txt's "initramfs" line says), place the buffer
     * dynamically, above whichever of {kernel image end, initrd end}
     * happens to be higher — computed fresh from real facts every boot,
     * not guessed once and left to bit-rot. */
    extern char _kernel_end[];
    uint64_t kernel_end_phys = (uint64_t)_kernel_end - KERNEL_VMA;
    uint64_t shim_buf_phys = kernel_end_phys > initrd_end ? kernel_end_phys : initrd_end;
    shim_buf_phys = align_up(shim_buf_phys, PAGE_SIZE);
    void *mb2_shim_buf = (void *)shim_buf_phys;
    uint64_t mb2_addr = mb2_shim_build(regions, n, mb2_shim_buf, 512, initrd_start, initrd_end);
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
 * stack instead of boot.S's temporary one, reached via a raw stack-
 * pointer switch + branch, never returning to kernel_stage1. Does
 * ONLY genuinely arch-specific hardware bring-up — exception vector
 * table, GIC-400, generic timer — then hands off to the shared
 * kernel_stage2() (kernel/kernel.c) for everything else (initrd, the
 * scheduler bring-up test), which never returns either.
 *
 * Order matters here: VBAR_EL1 must be live before anything that could
 * fault (including the GIC's own MMIO mapping), and the CPU-level IRQ
 * mask shouldn't lift until the GIC and timer are fully configured, so
 * nothing can be delivered before something exists to handle it.
 */
static void kernel_aarch64_stage2(uint64_t mb2_addr) {
    kprintf("--- AArch64 Kernel Stage 2 ---\n");
    kprintf("Successfully running on the VMM-allocated kernel stack.\n");

    exceptions_init();
    kprintf("aarch64: VBAR_EL1 set (synchronous exceptions verified separately).\n");

    gic_init();
    kprintf("aarch64: GIC-400 initialized (GICD 0xFF841000, GICC 0xFF842000).\n");

    /* 1kHz, so a sleep can resolve to a millisecond.
     *
     * 20Hz was fine while the only sleeper was a test payload asking for
     * whole seconds, but it makes any short sleep a 50ms sleep —
     * sys_sleep() rounds a sub-tick request up to one tick, so
     * DG_SleepMs(1) cost 50x what it asked for. DOOM's loop calls that
     * constantly while waiting for its next 28.6ms tic, so it overshot
     * every deadline and the whole game felt heavy.
     *
     * The cost is 1000 interrupts a second instead of 20; each one
     * ticks a counter, scans the process table for expired sleepers and
     * calls schedule(), all of which is trivial on a 1.5GHz A72. */
    timer_init(1000);
    kprintf("aarch64: generic timer armed at %u Hz, IRQ %d enabled.\n", timer_ticks_per_second(), GIC_PPI_NS_PHYS_TIMER);

    exceptions_enable_irqs();
    kprintf("aarch64: IRQs unmasked at the CPU.\n");

    kernel_stage2(mb2_addr);
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
