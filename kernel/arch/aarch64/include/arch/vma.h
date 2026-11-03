#ifndef ARCH_VMA_H
#define ARCH_VMA_H

/* Matches x86's higher-half offset — see kernel/arch/aarch64/boot.S for
 * the early bootstrap page tables that bridge from the low physical load
 * address RPi4 firmware always uses (0x80000) to this virtual base. */
#define KERNEL_VMA 0xFFFFFFFF80000000ULL

/* User address space bounds (TTBR0, 48-bit). */
#define ARCH_USER_VA_LIMIT 0x0001000000000000ULL
#define ARCH_USER_STACK_TOP 0x0000FFFFFFFF0000ULL

#endif
