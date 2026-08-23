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
#include <kernel/fs/ramfs.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/exec.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/tty.h>
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
    ramfs_init();
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

    /* The original 2-proc round-robin scheduler proof (user_test.elf,
     * driven by map_test_counter_page()/timer_set_counter_watch()) is
     * retired along with user_test.S — see git history for both if it's
     * ever needed again. Superseded by the mlibc fork/exec/wait suite's
     * own concurrency demonstration (usr/mlibc_tests/mlibc_fork_stress.c). */
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
    tty_init();

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

    /* The raw-syscall/hand-rolled-asm test payloads that used to be
     * parked here (sleep_test, spin_write_test, anon_alloc_test,
     * badptr_test, fp_test_a/b, uptime_test, libc_test, mem_stress,
     * heartbeat_test, read_echo_test, fb_user_test, fork_test — all
     * .S/.c files directly under usr/) are retired. Every one of them
     * either had its coverage superseded outright (DOOM + fib_ticker +
     * key_monitor together demonstrate real concurrency and framebuffer
     * use far more thoroughly than the smoke tests they replace) or was
     * ported to a real-mlibc equivalent under usr/mlibc_tests/:
     *   mlibc_badptr_test.c   — user_buffer_ok() pointer isolation
     *   mlibc_fp_test.c       — FP/SIMD + TPIDR_EL0 across preemption
     *   mlibc_mem_stress.c    — allocator load + OOM refusal paths
     *   mlibc_fork_stress.c   — fork/execve/wait + PMM leak check
     *   mlibc_syscall_test.c  — every syscall, exercised through mlibc
     * See git history for the retired originals if ever needed again.
     * All future usr/ tests are written against mlibc, not raw
     * syscalls or hand-rolled asm — see any of the files above for the
     * established pattern (a local raw_syscall() shim only for the
     * handful of extron-specific syscalls mlibc has no libc entry point
     * for at all).
     */

    /* Start the interactive system environment. BusyBox is a static mlibc
     * binary whose applets re-exec through /sh; the ramfs provides its
     * writable working tree while retaining initrd files as COW views. */
    struct proc *shell = proc_create_from_binary("sh", 0);
    if (!shell)
        panic("kernel_stage2: failed to create BusyBox ash");
    sched_policy_add(shell);

    /* The earlier concurrent demonstrations remain handy as optional
     * scheduler tests:
     *
     * struct proc *fib = proc_create_from_binary("fib_ticker.elf", 0);
     * if (!fib) {
     *     panic("kernel_stage2: failed to create the fibonacci proc");
     * }
     * sched_policy_add(fib);
     *
     * struct proc *keymon = proc_create_from_binary("key_monitor.elf", 0);
     * if (!keymon) {
     *     panic("kernel_stage2: failed to create the key monitor proc");
     * }
     * sched_policy_add(keymon);
     */

    kprintf("Starting scheduler — BusyBox ash.\n");
    sched_start();

    panic("kernel_stage2: sched_start() returned");
}
