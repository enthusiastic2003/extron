#include "mb2_shim.h"
#include <boot/multiboot2.h>

// memcpy-based field writes: correct regardless of buffer alignment or
// strict-aliasing, unlike writing through a (struct multiboot_tag_mmap *)
// cast over a raw uint8_t buffer.
static void put_u32(uint8_t *dst, uint32_t v) {
    __builtin_memcpy(dst, &v, sizeof(v));
}

static void put_u64(uint8_t *dst, uint64_t v) {
    __builtin_memcpy(dst, &v, sizeof(v));
}

uint64_t mb2_shim_build(const struct fdt_mem_region *regions, size_t count,
                         void *buf, size_t buf_size,
                         uint64_t initrd_start, uint64_t initrd_end) {
    uint32_t mmap_tag_size = (uint32_t)(sizeof(struct multiboot_tag_mmap) +
                                         count * sizeof(struct multiboot_mmap_entry));
    uint32_t mmap_tag_aligned = (mmap_tag_size + 7u) & ~7u;

    /* struct multiboot_tag_module's cmdline[] is a null-terminated string
     * per the multiboot2 spec — one byte (an empty string) is enough since
     * neither tar_init() nor anything else reads it; only mod_start/
     * mod_end matter to the initrd path. */
    int have_module = (initrd_start != 0 || initrd_end != 0);
    uint32_t module_tag_size = have_module
        ? (uint32_t)(sizeof(struct multiboot_tag_module) + 1)
        : 0;
    uint32_t module_tag_aligned = (module_tag_size + 7u) & ~7u;

    uint32_t total_size = 8 /* mb2 info header: total_size + reserved */
                        + mmap_tag_aligned
                        + module_tag_aligned
                        + 8 /* END tag */;

    if ((size_t)total_size > buf_size) {
        return 0;
    }

    uint8_t *p = (uint8_t *)buf;

    // --- multiboot2 info header (no dedicated struct in multiboot2.h;
    // pmm.c itself reads it as raw uint32_t at mb2_addr/mb2_addr+4) ---
    put_u32(p + 0, total_size);
    put_u32(p + 4, 0); // reserved

    // --- MMAP tag ---
    uint8_t *mmap = p + 8;
    put_u32(mmap + 0, MULTIBOOT_TAG_TYPE_MMAP);
    put_u32(mmap + 4, mmap_tag_size);
    put_u32(mmap + 8, (uint32_t)sizeof(struct multiboot_mmap_entry));
    put_u32(mmap + 12, 0); // entry_version

    uint8_t *entries = mmap + 16;
    for (size_t i = 0; i < count; i++) {
        uint8_t *e = entries + i * sizeof(struct multiboot_mmap_entry);
        put_u64(e + 0, regions[i].base);
        put_u64(e + 8, regions[i].size);
        put_u32(e + 16, MULTIBOOT_MEMORY_AVAILABLE);
        put_u32(e + 20, 0); // zero
    }

    // --- MODULE tag (the initrd, if one was found) ---
    if (have_module) {
        uint8_t *mod = p + 8 + mmap_tag_aligned;
        put_u32(mod + 0, MULTIBOOT_TAG_TYPE_MODULE);
        put_u32(mod + 4, module_tag_size);
        put_u32(mod + 8, (uint32_t)initrd_start);
        put_u32(mod + 12, (uint32_t)initrd_end);
        mod[16] = 0; // cmdline: empty string
    }

    // --- END tag ---
    uint8_t *end = p + 8 + mmap_tag_aligned + module_tag_aligned;
    put_u32(end + 0, MULTIBOOT_TAG_TYPE_END);
    put_u32(end + 4, 8);

    return (uint64_t)(uintptr_t)buf;
}
