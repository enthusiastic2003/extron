#ifndef AARCH64_MB2_SHIM_H
#define AARCH64_MB2_SHIM_H

#include <stdint.h>
#include <stddef.h>
#include "fdt.h"

/*
 * Packages FDT-derived memory regions into a real, spec-compliant
 * multiboot2 info blob (an MMAP tag, an optional MODULE tag, and an END
 * tag) inside the caller-provided buffer, so the existing x86
 * init_pmm(mb2_addr)/tar_init(mb2_addr) can consume it completely
 * unmodified. Pass initrd_start == initrd_end == 0 to omit the MODULE
 * tag entirely (no initrd found/loaded). Returns the blob's address
 * (== buf) on success, or 0 if buf is too small.
 */
uint64_t mb2_shim_build(const struct fdt_mem_region *regions, size_t count,
                         void *buf, size_t buf_size,
                         uint64_t initrd_start, uint64_t initrd_end);

#endif
