#include <kernel/console.h>
#include <kernel/panic.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/kheap.h>
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

/* Exercises kmalloc/kfree (kernel/mm/kheap.c — liballoc, already used
 * indirectly this whole time via proc_create_from_binary()'s struct
 * proc and kernel stack allocations, but never directly proven).
 * Allocates three DIFFERENT sizes (small, medium, and one bigger than
 * a single page — liballoc's allocate_new_page() rounds a request like
 * that up across multiple pages internally) so any block-boundary
 * miscalculation would show up as one allocation stomping another's
 * bytes. Frees the middle one specifically and allocates again to
 * prove the freed space is actually reusable, not just leaked. */
static void kheap_test(void) {
    kprintf("--- kheap test ---\n");

    char *a = kmalloc(16);
    char *b = kmalloc(128);
    char *c = kmalloc(5000); /* bigger than one 4KB page */

    if (!a || !b || !c) {
        panic("kheap test: allocation failed");
    }

    for (int i = 0; i < 16; i++)   a[i] = 'A';
    for (int i = 0; i < 128; i++)  b[i] = 'B';
    for (int i = 0; i < 5000; i++) c[i] = 'C';

    int ok = 1;
    for (int i = 0; i < 16; i++)   if (a[i] != 'A') ok = 0;
    for (int i = 0; i < 128; i++)  if (b[i] != 'B') ok = 0;
    for (int i = 0; i < 5000; i++) if (c[i] != 'C') ok = 0;

    kprintf("kheap test: a=%p b=%p c=%p pattern check %s\n",
            (void *)a, (void *)b, (void *)c, ok ? "PASSED" : "FAILED");

    kfree(b);

    char *d = kmalloc(64);
    if (!d) {
        panic("kheap test: post-free allocation failed");
    }
    for (int i = 0; i < 64; i++) d[i] = 'D';

    int ok2 = 1;
    for (int i = 0; i < 64; i++)   if (d[i] != 'D') ok2 = 0;
    for (int i = 0; i < 16; i++)   if (a[i] != 'A') ok2 = 0; /* untouched by the free/realloc above? */
    for (int i = 0; i < 5000; i++) if (c[i] != 'C') ok2 = 0;

    kprintf("kheap test: post-free reuse d=%p pattern check %s\n",
            (void *)d, ok2 ? "PASSED" : "FAILED");

    kfree(a);
    kfree(c);
    kfree(d);

    kprintf("--- kheap test done ---\n");
}

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
    proc_table_init();
    sched_init();

    kheap_test();

    /* Keyboard-echo test — parked for the syscall test below, same
     * "one test at a time" pattern as the 2-proc scheduler test above.
     * init_kbd() (kernel/drivers/keyboard.c) still runs either way: the
     * syscall test doesn't need it, but leaving interrupt-driven RX
     * armed is harmless and saves re-adding it once SYS_READ exists.
     *
    kprintf("Echo test: type on the serial console...\n");
    for (;;) {
        char c = kbd_getc();
        serial_putc(c);
    }
    */
    init_kbd();

    /* Phase 2 verification, sleep/wake + VMA allocator (already proven
     * on both QEMU and real hardware — see git history for that run):
     * struct proc *sleeper = proc_create_from_binary("sleep_test.elf");
     * struct proc *spinner = proc_create_from_binary("spin_write_test.elf");
     * struct proc *allocator = proc_create_from_binary("anon_alloc_test.elf");
     * sched_policy_add(sleeper); sched_policy_add(spinner); sched_policy_add(allocator);
     *
     * Now proving the last item from that same plan's verification
     * list: SYS_READ genuinely wakes a blocked reader on a real
     * keystroke rather than busy-spinning. read_echo_test.elf blocks
     * on SYS_READ and echoes each byte typed; heartbeat_test.elf
     * prints "." every ~200ms via SYS_SLEEP the whole time — if the
     * dots keep coming while nothing's been typed, and each keystroke
     * gets echoed promptly, that's kbd_getc()'s sleep()/wakeup() round
     * trip proven live, not just inferred. */
    struct proc *reader = proc_create_from_binary("read_echo_test.elf");
    struct proc *heartbeat = proc_create_from_binary("heartbeat_test.elf");
    /* Runs alongside them and exits: proves SYS_READ/SYS_WRITE reject
     * pointers into the kernel half or into unmapped pages, rather than
     * dereferencing them at EL1 (kernel/proc/syscall.c's
     * user_buffer_ok()). Prints three PASS/FAIL lines, then exits, so
     * the interactive test carries on afterwards uninterrupted. */
    struct proc *badptr = proc_create_from_binary("badptr_test.elf");
    /* Pair, not a single proc: each loads a different pattern into the
     * same FP registers and yields repeatedly, so a context_switch that
     * dropped FP state leaves each holding the other's values. One proc
     * alone would pass even with no FP save at all, since the kernel is
     * built -mgeneral-regs-only and never touches those registers. */
    struct proc *fp_a = proc_create_from_binary("fp_test_a.elf");
    struct proc *fp_b = proc_create_from_binary("fp_test_b.elf");
    /* Cross-checks the two independent clocks against each other:
     * SYS_SLEEP counts 20Hz timer interrupts, SYS_UPTIME_MS reads
     * CNTPCT_EL0. Sleeping a known interval and measuring it with the
     * other source validates both. */
    struct proc *uptime = proc_create_from_binary("uptime_test.elf");
    if (!reader || !heartbeat || !badptr || !fp_a || !fp_b || !uptime) {
        panic("kernel_stage2: failed to create SYS_READ test procs");
    }
    sched_policy_add(reader);
    sched_policy_add(heartbeat);
    sched_policy_add(badptr);
    sched_policy_add(fp_a);
    sched_policy_add(fp_b);
    sched_policy_add(uptime);

    kprintf("Starting scheduler with 6 procs (SYS_READ + badptr + FP + uptime tests) — type on the console.\n");
    sched_start();

    panic("kernel_stage2: sched_start() returned");
}
