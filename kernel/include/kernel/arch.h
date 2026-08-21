#ifndef KERNEL_ARCH_H
#define KERNEL_ARCH_H

#include <stdint.h>

/*
 * Contract every architecture must implement exactly once, under
 * kernel/arch/<arch>/. Lets otherwise-shared kernel code (kernel.c,
 * console.c, panic.c) stay genuinely arch-neutral instead of carrying
 * #ifdef __x86_64__ blocks inline.
 */

/* kernel.c: kernel_stage1's arch-specific bookends. early runs before
 * init_pmm(); late runs after and never returns (halts, or on x86,
 * transitions into kernel_stage2). */
void arch_kernel_early_init(void);
void arch_kernel_late_init(uint64_t mb2_addr) __attribute__((noreturn));

/* panic.c */
void arch_disable_interrupts(void);
void arch_halt_forever(void) __attribute__((noreturn));

/* console.c: hardware text-mode cursor update, if the arch has one. */
void arch_update_hw_cursor(int x, int y);

#endif
