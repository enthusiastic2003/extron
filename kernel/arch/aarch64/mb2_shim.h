#ifndef AARCH64_MB2_SHIM_H
#define AARCH64_MB2_SHIM_H

#include <stdint.h>
#include <stddef.h>
#include "fdt.h"

/*
 * Packages FDT-derived memory regions into a real, spec-compliant
 * multiboot2 info blob (an MMAP tag + END tag) inside the caller-provided
 * buffer, so the existing x86 init_pmm(mb2_addr) can consume it completely
 * unmodified. Returns the blob's address (== buf) on success, or 0 if buf
 * is too small.
 */
uint64_t mb2_shim_build(const struct fdt_mem_region *regions, size_t count,
                         void *buf, size_t buf_size);

#endif
