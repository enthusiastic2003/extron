#ifndef ARCH_VMA_H
#define ARCH_VMA_H

/* No higher-half yet — flat/physical-linked until paging brings up a real
 * kernel virtual base (see kernel/arch/aarch64/paging_aarch64.c). */
#define KERNEL_VMA 0ULL

#endif
