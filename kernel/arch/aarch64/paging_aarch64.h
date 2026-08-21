#ifndef AARCH64_PAGING_H
#define AARCH64_PAGING_H

#include <stdint.h>

/*
 * Milestone 3, minimal first pass: identity-map (VA == PA) all discovered
 * RAM plus the UART/GPIO MMIO page under TTBR0_EL1, then enable the MMU.
 * No higher-half yet, no user/kernel split (TTBR1_EL1 walks are disabled
 * via TCR_EL1.EPD1) — see kernel/arch/aarch64/paging_aarch64.c for why.
 *
 * mb2_addr is the same multiboot2-shaped pointer kernel_stage1 already
 * has (see mb2_shim.c) — this walks its MMAP tag directly rather than
 * threading a separate region list through the shared entry point.
 */
void aarch64_paging_init(uint64_t mb2_addr);

#endif
