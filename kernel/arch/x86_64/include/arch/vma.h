#ifndef ARCH_VMA_H
#define ARCH_VMA_H

/* Higher-half kernel base. boot.S bridges from the low physical load
 * address chosen by the UEFI loader (see boot/uefi/) to this virtual base. */
#define KERNEL_VMA 0xFFFFFFFF80000000ULL

/* User address space bounds: the lower canonical half (47 bits). */
#define ARCH_USER_VA_LIMIT 0x0000800000000000ULL
#define ARCH_USER_STACK_TOP 0x00007FFFFFFF0000ULL

#endif
