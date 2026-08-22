#include <kernel/console.h>
#include <kernel/panic.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
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
 * function directly).
 *
 * init_pmm/init_paging/vmm_init/vmm_setup_stack are the SAME functions
 * (same headers, same call sites) on both architectures, so they stay
 * directly in this shared function rather than being duplicated behind
 * arch-specific hooks. Only the genuinely divergent bits — early
 * hardware bring-up (IDT/PIC/GDT on x86; nothing yet on aarch64) and the
 * raw-asm stack-switch-and-jump into each arch's own Stage 2 (the
 * instructions themselves differ per ISA, can't be shared) — go through
 * kernel/include/kernel/arch.h, implemented once per arch under
 * kernel/arch/x86_64/ and kernel/arch/aarch64/.
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
