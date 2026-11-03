#include <kernel/drivers/serial.h>
#include <kernel/drivers/timer.h>
#include <kernel/console.h>
#include <kernel/panic.h>
#include <kernel/arch.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
#include <arch/exceptions.h>
#include <arch/irq.h>
#include <arch/vma.h>
#include <kernel/klibc/string.h>

/*
 * x86-64 entry, called from boot.S with the physical address of the
 * multiboot2-shaped boot info block built by the UEFI loader (memory map
 * + initrd module). From here the flow is exactly the aarch64 one:
 * kernel_stage1() (arch-neutral) -> arch_kernel_jump_to_stage2() ->
 * this file's stage2 (interrupt controller, timer, IRQs on) ->
 * kernel_stage2() (arch-neutral: VFS, initrd root, first process).
 */
extern void kernel_stage1(uint64_t mb2_addr);
extern void kernel_stage2(uint64_t mb2_addr) __attribute__((noreturn));

void kernel_x86_64_main(uint64_t mb2_phys) {
    init_serial();
    kprintf("\n=======================================\n");
    kprintf(" Extron OS: x86-64 (UEFI) boot OK\n");
    kprintf("=======================================\n");
    kernel_stage1(mb2_phys);
    for (;;) __asm__ volatile ("cli; hlt");
}

/* --- kernel/include/kernel/arch.h contract --- */

void arch_disable_interrupts(void) { __asm__ volatile ("cli"); }

void arch_halt_forever(void) {
    for (;;) __asm__ volatile ("cli; hlt");
}

void arch_update_hw_cursor(int x, int y) { (void)x; (void)y; }

void arch_kernel_early_init(void) {
    /* The IDT must exist before anything can fault; IRQ lines stay masked. */
    exceptions_init();
    pic_remap();
}

void arch_kernel_mid_init(void) {}

/* One-line boot-time sanity/throughput check of the rep-string mem* routines
 * against a plain byte loop (the previous implementation). */
static void mem_bench(void) {
    enum { SZ = 4 << 20 };
    static uint8_t a[SZ], b[SZ];
    volatile uint8_t *va = a, *vb = b;
    uint64_t t0 = timer_uptime_ns();
    for (int i = 0; i < 8; i++) memcpy(b, a, SZ);
    uint64_t t1 = timer_uptime_ns();
    for (size_t i = 0; i < SZ; i++) vb[i] = va[i];
    uint64_t t2 = timer_uptime_ns();
    for (int i = 0; i < 8; i++) memset(b, i, SZ);
    uint64_t t3 = timer_uptime_ns();
    uint64_t fast = (t1 - t0) ? (8ULL * SZ * 1000) / (t1 - t0) : 0;   /* MB/s */
    uint64_t slow = (t2 - t1) ? ((uint64_t)SZ * 1000) / (t2 - t1) : 0;
    uint64_t fset = (t3 - t2 ? t3 - t2 : 1) ? (8ULL * SZ * 1000) / (t3 - t2) : 0;
    kprintf("mem: memcpy %lu MB/s, memset %lu MB/s (byte loop: %lu MB/s)\n",
            (unsigned long)fast, (unsigned long)fset, (unsigned long)slow);
}

static void kernel_x86_64_stage2(uint64_t mb2_addr) {
    kprintf("--- x86-64 Kernel Stage 2 ---\n");
    kprintf("Successfully running on the VMM-allocated kernel stack.\n");

    timer_init(1000);
    kprintf("x86_64: PIT armed at %u Hz, IRQ0 unmasked at the PIC.\n", timer_ticks_per_second());

    mem_bench();
    exceptions_enable_irqs();
    kprintf("x86_64: interrupts enabled.\n");

    kernel_stage2(mb2_addr);
}

void arch_kernel_jump_to_stage2(uint64_t mb2_addr, uint64_t new_stack_top) {
    /* The loader's low identity map is gone once init_paging() installed the
     * kernel PML4, so the boot info block is reached through the HHDM. */
    uint64_t mb2_virt = mb2_addr + NEW_HDDM;
    extern void fb_set_boot_info(uint64_t);
    fb_set_boot_info(mb2_virt);
    __asm__ volatile (
        "lea -8(%1), %%rsp\n\t"
        "xor %%ebp, %%ebp\n\t"
        "jmp *%2"
        :: "D"(mb2_virt), "r"(new_stack_top), "r"(kernel_x86_64_stage2)
        : "memory");
    for (;;) __asm__ volatile ("hlt");
}
