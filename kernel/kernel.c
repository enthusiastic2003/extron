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
#include <kernel/drivers/mailbox.h>
#include <kernel/drivers/fb.h>

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
__attribute__((unused)) static void kheap_test(void) {
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
    struct proc *proc_a = proc_create_from_binary("user_test.elf", 0);
    struct proc *proc_b = proc_create_from_binary("user_test.elf", 0);
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

    /* Proven; parked with the rest of the test payloads to keep the
     * console clear. Uncomment to re-run. */
    /* kheap_test(); */

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

    /* VideoCore mailbox bring-up. Deliberately before any framebuffer
     * code: it proves the transport (Device mapping, cache maintenance,
     * bus-address translation, tag walk) on its own, so a later
     * framebuffer failure can't be confused with a broken channel. The
     * VC memory answer should match the hole the memory map leaves
     * between its two available regions. */
    mailbox_init();
    mailbox_report();

    /* Ask for 640x480 — a mode this display can actually present. The
     * capture showed it negotiating 4:3, so requesting 16:9 would just
     * get something else back; either way the values the firmware
     * REPORTS are what fb.c uses. The test pattern is designed so the
     * usual mistakes look different from each other: skewed bars mean
     * pitch, misplaced corners mean geometry, permuted colours mean
     * byte order. */
    if (fb_init(640, 480)) {
        fb_test_pattern();
    }

    /* Verification payloads, all proven on QEMU and real hardware and
     * parked to keep the console readable while the framebuffer is the
     * thing under test. Same "one test at a time" pattern as the earlier
     * scheduler and keyboard-echo tests above; uncomment a block to
     * bring it back.
     *
     * Phase 2 — sleep/wake + VMA allocator:
     *   sleep_test.elf, spin_write_test.elf, anon_alloc_test.elf
     * Syscall pointer validation (user_buffer_ok):
     *   badptr_test.elf — 3 checks
     * Per-process CPU state across context switches. A PAIR, because one
     * proc passes even with no save at all:
     *   fp_test_a.elf + fp_test_b.elf — v0-v31, FPCR/FPSR, TPIDR_EL0
     * The two independent clocks cross-checking each other (SYS_SLEEP
     * counts 20Hz ticks, SYS_UPTIME_MS reads CNTPCT_EL0):
     *   uptime_test.elf
     * Userspace C: crt0 -> main -> libc -> syscalls -> exit, plus the
     * shared liballoc on user pages, and SYS_MAP_INITRD:
     *   libc_test.elf — 21 checks
     * 8192 page allocations plus the out-of-memory refusal paths:
     *   mem_stress.elf — 8 checks
     *
     * struct proc *badptr    = proc_create_from_binary("badptr_test.elf", 0);
     * struct proc *fp_a      = proc_create_from_binary("fp_test_a.elf", 0);
     * struct proc *fp_b      = proc_create_from_binary("fp_test_b.elf", 0);
     * struct proc *uptime    = proc_create_from_binary("uptime_test.elf", 0);
     * struct proc *libc      = proc_create_from_binary("libc_test.elf", 0);
     * struct proc *memstress = proc_create_from_binary("mem_stress.elf", 0);
     * sched_policy_add(badptr);  sched_policy_add(fp_a);
     * sched_policy_add(fp_b);    sched_policy_add(uptime);
     * sched_policy_add(libc);    sched_policy_add(memstress);
     *
     * heartbeat_test.elf stays parked too — its ~5 dots/second is exactly
     * the clutter this is about, and its job (proving a blocked reader
     * really yields the CPU) is done.
     *
     * struct proc *heartbeat = proc_create_from_binary("heartbeat_test.elf", 0);
     * sched_policy_add(heartbeat);
     */

    /* One proc left running: it blocks in SYS_READ and echoes what you
     * type. Silent until then, so it costs nothing on the console while
     * still showing the system is alive and responsive — and with
     * nothing else runnable it parks the CPU in schedule()'s wfi idle
     * path (e6acf26) rather than spinning. */
    /* The console echo proc is parked now that DOOM is the payload: it
     * reads the same keystrokes DOOM does (the ISR feeds both the
     * blocking kbuf and the shared ring) and echoes them to serial,
     * which turns every movement key into console noise.
     *
     * struct proc *reader = proc_create_from_binary("read_echo_test.elf", 0);
     * sched_policy_add(reader);
     */

    /* Userspace framebuffer smoke test — proven, parked alongside the
     * rest now that DOOM exercises the same path for real.
     *
     * struct proc *fbuser = proc_create_from_binary("fb_user_test.elf",
     *                                               PROC_MAP_FRAMEBUFFER);
     * sched_policy_add(fbuser);
     */

    /* DOOM. PROC_MAP_FRAMEBUFFER hands it the display and the keystroke
     * ring at creation, so its frame loop makes no syscall to draw and
     * none to poll input — only SYS_SLEEP to pace itself and
     * SYS_UPTIME_MS for its clock. The WAD comes out of the initrd via
     * SYS_MAP_INITRD, mapped rather than copied. */
    struct proc *doom = proc_create_from_binary("doom.elf",
                                                PROC_MAP_FRAMEBUFFER);
    if (!doom) {
        panic("kernel_stage2: failed to create the DOOM proc");
    }
    sched_policy_add(doom);

    kprintf("Starting scheduler — type on the console to echo.\n");
    sched_start();

    panic("kernel_stage2: sched_start() returned");
}
