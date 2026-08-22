#ifndef KERNEL_ARCH_H
#define KERNEL_ARCH_H

#include <stdint.h>

/*
 * Contract every architecture must implement exactly once, under
 * kernel/arch/<arch>/. Lets otherwise-shared kernel code (kernel.c,
 * console.c, panic.c) stay genuinely arch-neutral instead of carrying
 * #ifdef __x86_64__ blocks inline.
 */

/* kernel.c: kernel_stage1's arch-specific hooks. The shared, arch-neutral
 * sequence (init_pmm, init_paging, vmm_init, vmm_setup_stack — same
 * functions/headers on every arch) stays directly in kernel_stage1
 * itself; only the genuinely divergent bookends live here:
 *   - early:  runs before init_pmm() (x86: idt_init+pic_remap; aarch64: nothing yet)
 *   - mid:    runs after init_pmm(), before init_paging() (x86: gdt_reload; aarch64: nothing yet)
 *   - jump_to_stage2: the raw stack-switch-and-branch into each arch's own
 *     Stage 2 entry point — can't be shared, the asm itself differs per ISA. */
void arch_kernel_early_init(void);
void arch_kernel_mid_init(void);
void arch_kernel_jump_to_stage2(uint64_t mb2_addr, uint64_t new_stack_top) __attribute__((noreturn));

/* panic.c */
void arch_disable_interrupts(void);
void arch_halt_forever(void) __attribute__((noreturn));

/* console.c: hardware text-mode cursor update, if the arch has one. */
void arch_update_hw_cursor(int x, int y);

#endif
