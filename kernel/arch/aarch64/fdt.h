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

/*
 * Reads the /chosen node's "linux,initrd-start"/"linux,initrd-end"
 * properties (the standard ARM/Linux boot-protocol convention a
 * bootloader uses to hand off an initrd's physical bounds) — the same
 * mechanism RPi4 firmware's config.txt "initramfs" directive uses.
 * Returns 1 and fills out_start/out_end if both properties were found,
 * 0 otherwise (no initrd was loaded, or dtb_phys isn't a valid FDT).
 */
int fdt_get_initrd_region(const void *dtb_phys, uint64_t *out_start, uint64_t *out_end);

/*
 * The FDT header's own totalsize field — how many bytes of RAM the
 * device tree blob occupies, so it can be reserved from the physical
 * allocator. Returns 0 if dtb_phys isn't a valid FDT.
 */
uint32_t fdt_get_total_size(const void *dtb_phys);

#endif
