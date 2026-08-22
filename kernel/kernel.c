#include <kernel/console.h>
#include <kernel/panic.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
#include <kernel/arch.h>
#include <stddef.h>
#include <kernel/drivers/serial.h>
#include <kernel/drivers/timer.h>
#include <kernel/fs/tar.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/exec.h>
#include <kernel/drivers/keyboard.h>

/**
 * @brief Stage 1: Initialization Phase.
 * Runs on the temporary boot stack.
 *
 * Arch-neutral. mb2_addr is a synthesized multiboot2 MMAP tag built from
 * FDT-derived memory regions (see kernel/arch/aarch64/mb2_shim.c and
 * kernel_aarch64_main, which calls this function directly) — kept in
 * multiboot2 shape purely so init_pmm/tar_init and friends don't need
 * two different memory-map formats to understand.
 *
 * init_pmm/init_paging/vmm_init/vmm_setup_stack stay directly in this
 * function rather than behind arch-specific hooks, since they're
 * already arch-neutral (same headers, same call sites, arch-specific
 * internals live behind kernel/mm/paging.h's own contract). Only the
 * genuinely divergent bits — early hardware bring-up before paging
 * exists, and the raw-asm stack-switch-and-jump into Stage 2 (the
 * instructions themselves are ISA-specific) — go through
 * kernel/include/kernel/arch.h, implemented under kernel/arch/aarch64/.
 * kernel_stage2() below is reached from there once that arch-specific
 * hardware bring-up (interrupt controller, timer) finishes — see
 * kernel_aarch64_stage2()'s own comment.
 */
void kernel_stage1(uint64_t mb2_addr) {
    init_serial();

    kprintf("--- Kernel Stage 1: Initialization ---\n");

    arch_kernel_early_init();
    init_pmm(mb2_addr);
    arch_kernel_mid_init();
    init_paging(mb2_addr);
    pmm_print_stats();
    vmm_init();

    virt_addr_t new_stack_top = vmm_setup_stack();

    kprintf("Transitioning to Stage 2...\n");

    arch_kernel_jump_to_stage2(mb2_addr, new_stack_top); /* noreturn */
}

/* Test-only, parked alongside its only caller in kernel_stage2() below
 * (the commented-out 2-proc scheduler test) — maps a fresh physical
 * page into `p`'s own address space at a fixed VA, purely so its EL0
 * code (user_test.elf's counter-increment loop) can write progress
 * somewhere the kernel can independently read back via
 * phys_to_virt_hhdm() — the same access pattern already used for the
 * PMM bitmap and the initrd — without either proc making a syscall.
 * Not part of proc_create_from_binary() (kernel/proc/exec.c): no future
 * real process needs a kernel-observable scratch page, this is
 * specific to proving the scheduler round-robins correctly. Returns
 * the page's physical address for timer_set_counter_watch().
 *
static phys_addr_t map_test_counter_page(struct proc *p) {
    phys_addr_t counter_phys = (phys_addr_t)pmm_alloc_page();
    *(volatile uint64_t *)phys_to_virt_hhdm(counter_phys) = 0;
    map_page(p->ttbr0, 0x600000, counter_phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX);
    return counter_phys;
}
*/

/**
 * @brief Stage 2: arch-neutral continuation.
 *
 * Called once each arch's own Stage 2 (kernel_aarch64_stage2() on this
 * side) finishes genuinely arch-specific hardware bring-up (interrupt
 * controller, timer, exceptions unmasked) — everything from here on
 * touches no arch-specific register or instruction, only the same
 * portable calls (tar_open, proc_create_from_binary, sched_*) any
 * future architecture would use identically. Never returns.
 */
void kernel_stage2(uint64_t mb2_addr) {
    /* Proves the initrd delivery pipeline end-to-end: DTB /chosen ->
     * fdt_get_initrd_region() (aarch64-specific) -> a real or synthesized
     * multiboot2 MODULE tag -> tar_init(). */
    tar_init(mb2_addr);
    tar_list();
    struct tar_file f;
    if (tar_open("hello.txt", &f)) {
        kprintf("hello.txt (%u bytes): \"", (unsigned)f.size);
        const char *bytes = (const char *)f.data;
        for (size_t i = 0; i < f.size; i++) {
            kprintf("%c", bytes[i]);
        }
        kprintf("\"\n");
    } else {
        kprintf("hello.txt not found in initrd.\n");
    }

    /* Scheduler bring-up test — parked for the keyboard-echo test below,
     * no processes created this time. See git history / uncomment to
     * bring back the 2-proc round-robin proof.
     *
    struct proc *proc_a = proc_create_from_binary("user_test.elf");
    struct proc *proc_b = proc_create_from_binary("user_test.elf");
    if (!proc_a || !proc_b) {
        panic("kernel_stage2: failed to create scheduler test procs");
    }

    phys_addr_t counter_phys_a = map_test_counter_page(proc_a);
    phys_addr_t counter_phys_b = map_test_counter_page(proc_b);

    sched_policy_add(proc_a);
    sched_policy_add(proc_b);
    timer_set_counter_watch(counter_phys_a, counter_phys_b);

    kprintf("Starting scheduler with 2 procs.\n");
    sched_start();
    */
    sched_init();

    /* Keyboard-echo test: read bytes from the serial console
     * (kernel/drivers/keyboard.c — really just UART RX on this headless
     * setup) and write them straight back out. Proves RX end-to-end
     * before building anything that actually consumes typed input. */
    init_kbd();
    kprintf("Echo test: type on the serial console...\n");
    for (;;) {
        char c = kbd_getc();
        serial_putc(c);
    }
}
