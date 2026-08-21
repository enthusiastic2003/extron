#ifndef AARCH64_FDT_H
#define AARCH64_FDT_H

#include <stdint.h>
#include <stddef.h>

struct fdt_mem_region {
    uint64_t base;
    uint64_t size;
};

/*
 * Walks the flattened device tree at dtb_phys looking for the /memory
 * node's "reg" property (pairs of 64-bit base/size, big-endian, assuming
 * the standard aarch64 #address-cells/#size-cells = 2/2). Writes up to
 * max_regions entries into out and returns how many were found, or 0 if
 * dtb_phys doesn't point at a valid FDT.
 */
size_t fdt_get_memory_regions(const void *dtb_phys, struct fdt_mem_region *out, size_t max_regions);

#endif
