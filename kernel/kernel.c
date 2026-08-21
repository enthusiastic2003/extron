#include <kernel/console.h>
#include <kernel/panic.h>
#include <kernel/mm/pmm.h>
#include <kernel/arch.h>
#include <stddef.h>
#include <kernel/drivers/serial.h>

/**
 * @brief Stage 1: Initialization Phase.
 * Runs on the temporary boot stack.
 *
 * Shared entry point for both x86_64 and aarch64. mb2_addr is either the
 * real GRUB multiboot2 pointer (x86) or a synthesized multiboot2 MMAP tag
 * built from FDT-derived memory regions (aarch64 — see
 * kernel/arch/aarch64/mb2_shim.c and kernel_aarch64_main, which calls this
 * function directly). PMM bring-up is identical on both; everything
 * genuinely arch-specific lives behind arch_kernel_early_init()/
 * arch_kernel_late_init() (kernel/include/kernel/arch.h), implemented once
 * per arch under kernel/arch/x86_64/ and kernel/arch/aarch64/.
 */
void kernel_stage1(uint64_t mb2_addr) {
    init_serial();

    kprintf("--- Kernel Stage 1: Initialization ---\n");

    arch_kernel_early_init();
    init_pmm(mb2_addr);
    arch_kernel_late_init(mb2_addr); /* noreturn */
}
