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

    timer_init(2);
    kprintf("aarch64: generic timer armed at 2 Hz, IRQ %d enabled.\n", GIC_PPI_NS_PHYS_TIMER);

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

    /* TEMPORARY: Milestone 6 — drop to EL0 running a REAL ELF binary,
     * loaded through kernel/proc/elf_loader.c completely unmodified
     * (only kernel/include/kernel/elf.h's e_machine check needed a
     * per-arch value; see ELF_EXPECTED_MACHINE). Supersedes the earlier
     * hand-mapped-stub proof (see git history) now that there's a real
     * loader to exercise instead — same underlying eret mechanism,
     * proven for the second time on binary bytes we didn't hand-place
     * ourselves. user_test.elf is a throwaway, no-libc static binary
     * (no aarch64-extron toolchain exists yet) built directly with
     * aarch64-linux-gnu-gcc, not part of this build. */
    kprintf("aarch64: loading user_test.elf via the ELF loader...\n");

    struct tar_file elf_file;
    if (!tar_open("user_test.elf", &elf_file)) {
        panic("aarch64: user_test.elf not found in initrd");
    }

    phys_addr_t user_pml4 = create_user_pml4();
    virt_addr_t user_entry_va;
    int load_result = parse_and_load_binary((virt_addr_t)elf_file.data, elf_file.size,
                                             user_pml4, &user_entry_va);
    if (load_result != 0) {
        panic("aarch64: ELF load failed (%d)", load_result);
    }

    /* SP_EL0 needs somewhere valid to point, even though this stub never
     * pushes anything — an unmapped SP is still architecturally live the
     * instant we're at EL0. */
    phys_addr_t user_stack_phys = (phys_addr_t)pmm_alloc_page();
    uint64_t user_stack_va = 0x500000;
    map_page(user_pml4, user_stack_va, user_stack_phys,
             PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX);

    /* uart.c's serial_putc() talks to the UART by raw physical address
     * (no HHDM there) and has only ever worked because boot.S's
     * ORIGINAL TTBR0 identity table happened to cover it — the table
     * this test is about to replace outright. Without re-mapping the
     * same page here, every kprintf() after the switch below, including
     * this test's own result, goes silently into the void. */
    uint64_t uart_page = 0xFE201000ULL & ~(PAGE_SIZE - 1);
    map_page(user_pml4, uart_page, uart_page, PAGE_PRESENT | PAGE_WRITE | PAGE_CACHE_DISABLE);

    __asm__ volatile ("msr ttbr0_el1, %0" :: "r"(user_pml4) : "memory");
    flush_tlb();

    /* SPSR_EL1 M[3:0]=0x0 selects EL0t as the eret target, all DAIF bits
     * clear (same mask state we're already running under — the timer
     * IRQ stays live the instant we're at EL0, which is exactly why the
     * Lower-EL IRQ/FIQ vector slots got wired up above, not left
     * spurious). */
    register uint64_t elr    __asm__("x0") = user_entry_va;
    register uint64_t spsr   __asm__("x1") = 0x0;
    register uint64_t sp_el0 __asm__("x2") = user_stack_va + PAGE_SIZE;
    __asm__ volatile (
        "msr elr_el1, %0\n\t"
        "msr spsr_el1, %1\n\t"
        "msr sp_el0, %2\n\t"
        "eret"
        :: "r"(elr), "r"(spsr), "r"(sp_el0)
        : "memory"
    );

    kprintf("aarch64: unreachable — eret returned without trapping\n");

    for (;;) {
        __asm__ volatile ("wfe");
    }
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
