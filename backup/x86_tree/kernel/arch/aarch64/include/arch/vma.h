#ifndef ARCH_VMA_H
#define ARCH_VMA_H

/* Matches x86's higher-half offset — see kernel/arch/aarch64/boot.S for
 * the early bootstrap page tables that bridge from the low physical load
 * address RPi4 firmware always uses (0x80000) to this virtual base. */
#define KERNEL_VMA 0xFFFFFFFF80000000ULL

#endif
