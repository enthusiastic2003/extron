#include <kernel/drivers/serial.h>
#include <kernel/console.h>
#include <kernel/panic.h>
#include <kernel/arch.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
#include <arch/exceptions.h>
#include <arch/gic.h>
#include <arch/timer.h>
#include <arch/vma.h>
#include <kernel/fs/tar.h>
#include <kernel/proc/elf_loader.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
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

/*
 * Loads user_test.elf into a fresh address space for the scheduler
 * bring-up test (see kernel_aarch64_stage2()): its own ELF mapping, its
 * own stack page, its own counter page (shared with the kernel via
 * phys_to_virt_hhdm() — the same access pattern already used for the
 * PMM bitmap and the initrd — so progress is observable without either
 * proc making a syscall), and the UART page (needed in EVERY proc's own
 * table now that TTBR0_EL1 actually gets swapped between them — see the
 * single-process version of this comment, earlier in git history, for
 * why serial_putc() breaks without it). Returns the counter page's
 * physical address for timer_set_counter_watch().
 */
static phys_addr_t setup_test_proc(struct proc *p, uint64_t pid, uint64_t uart_page) {
    struct tar_file elf_file;
    if (!tar_open("user_test.elf", &elf_file)) {
        panic("aarch64: user_test.elf not found in initrd");
    }

    phys_addr_t pml4 = create_user_pml4();
    virt_addr_t entry;
    if (parse_and_load_binary((virt_addr_t)elf_file.data, elf_file.size, pml4, &entry) != 0) {
        panic("aarch64: ELF load failed for PID %lu", (unsigned long)pid);
    }

    phys_addr_t stack_phys = (phys_addr_t)pmm_alloc_page();
    uint64_t stack_va = 0x500000;
    map_page(pml4, stack_va, stack_phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX);

    phys_addr_t counter_phys = (phys_addr_t)pmm_alloc_page();
    *(volatile uint64_t *)phys_to_virt_hhdm(counter_phys) = 0;
    uint64_t counter_va = 0x600000;
    map_page(pml4, counter_va, counter_phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX);

    map_page(pml4, uart_page, uart_page, PAGE_PRESENT | PAGE_WRITE | PAGE_CACHE_DISABLE);

    proc_init(p, pid, entry, stack_va + PAGE_SIZE, pml4);
    return counter_phys;
}

/**
 * @brief aarch64 Stage 2: runs on the permanent VMM-allocated kernel
 * stack instead of boot.S's temporary one. The aarch64 counterpart to
 * kernel_stage2() in kernel/arch/x86_64/kernel_x86.c — reached the same
 * way, via a raw stack-pointer switch + branch, never returning to
 * kernel_stage1. mb2_addr now feeds tar_init() (kernel/fs/tar.c),
 * completely unmodified from the x86 side — see kernel_aarch64_main()'s
 * comment on how the initrd's bounds get into the mb2 shim in the first
 * place.
 *
 * Exception vector table + GIC-400 + timer bring-up all happen here,
 * in that order: VBAR_EL1 must be live before anything that could fault
 * (including the GIC's own MMIO mapping), and the CPU-level IRQ mask
 * shouldn't lift until the GIC and timer are fully configured, so
 * nothing can be delivered before something exists to handle it.
 */
static void kernel_aarch64_stage2(uint64_t mb2_addr) {
    kprintf("--- AArch64 Kernel Stage 2 ---\n");
    kprintf("Successfully running on the VMM-allocated kernel stack.\n");

    exceptions_init();
    kprintf("aarch64: VBAR_EL1 set (synchronous exceptions verified separately).\n");

    gic_init();
    kprintf("aarch64: GIC-400 initialized (GICD 0xFF841000, GICC 0xFF842000).\n");

    timer_init(20);
    kprintf("aarch64: generic timer armed at 20 Hz, IRQ %d enabled.\n", GIC_PPI_NS_PHYS_TIMER);

    exceptions_enable_irqs();
    kprintf("aarch64: IRQs unmasked at the CPU.\n");

    /* Proves the initrd delivery pipeline end-to-end: DTB /chosen ->
     * fdt_get_initrd_region() -> mb2 MODULE tag -> tar_init() (unmodified
     * from kernel/fs/tar.c, the exact same function x86 calls). */
    tar_init(mb2_addr);
    tar_list();
    struct tar_file f;
    if (tar_open("hello.txt", &f)) {
        kprintf("aarch64: hello.txt (%u bytes): \"", (unsigned)f.size);
        const char *bytes = (const char *)f.data;
        for (size_t i = 0; i < f.size; i++) {
            kprintf("%c", bytes[i]);
        }
        kprintf("\"\n");
    } else {
        kprintf("aarch64: hello.txt not found in initrd.\n");
    }

    /* Scheduler bring-up: two procs, same ELF (user_test.elf — a tight
     * counter-increment loop, no syscalls involved) loaded into two
     * separate address spaces, round-robin preempted by the timer IRQ.
     * Builds directly on the ELF-loader proof above: same
     * parse_and_load_binary()/create_user_pml4()/map_page() calls, just
     * done twice and handed to the scheduler instead of eret'd into
     * directly. See kernel/proc/sched.c for the actual switch
     * mechanism (schedule(), the forkret-style bootstrap trampoline). */
    uint64_t uart_page = 0xFE201000ULL & ~(PAGE_SIZE - 1);

    static struct proc proc_a, proc_b;
    phys_addr_t counter_phys_a = setup_test_proc(&proc_a, 0, uart_page);
    phys_addr_t counter_phys_b = setup_test_proc(&proc_b, 1, uart_page);

    sched_init();
    sched_policy_add(&proc_a);
    sched_policy_add(&proc_b);
    timer_set_counter_watch(counter_phys_a, counter_phys_b);

    kprintf("aarch64: starting scheduler with 2 procs.\n");
    sched_start();

    panic("aarch64: sched_start() returned");
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
