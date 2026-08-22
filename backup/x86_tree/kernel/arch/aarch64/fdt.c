#include "fdt.h"

// Minimal flattened-device-tree (FDT) walker. Only understands enough of
// the format (magic/header, BEGIN_NODE/END_NODE/PROP/NOP/END tokens) to
// find the /memory node's "reg" property. Not a general FDT library.

#define FDT_MAGIC       0xd00dfeedu

#define FDT_BEGIN_NODE  0x00000001u
#define FDT_END_NODE    0x00000002u
#define FDT_PROP        0x00000003u
#define FDT_NOP         0x00000004u
#define FDT_END         0x00000009u

static inline uint32_t read_be32(const void *p) {
    uint32_t v;
    __builtin_memcpy(&v, p, sizeof(v));
    return __builtin_bswap32(v);
}

static inline uint32_t align4(uint32_t x) {
    return (x + 3u) & ~3u;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

// Reads ncells consecutive big-endian 32-bit words as one big-endian value
// (how the devicetree spec encodes any #address-cells/#size-cells > 1).
static uint64_t read_cells(const uint8_t *p, uint32_t ncells) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < ncells; i++) {
        v = (v << 32) | read_be32(p + i * 4);
    }
    return v;
}

// Matches a node name of exactly "memory" or "memory@<unit-address>".
static int name_is_memory_node(const char *name) {
    static const char prefix[] = "memory";
    for (int i = 0; prefix[i]; i++) {
        if (name[i] != prefix[i]) {
            return 0;
        }
    }
    return name[6] == '\0' || name[6] == '@';
}

// Matches a node name of exactly "chosen" (never has a unit-address suffix).
static int name_is_chosen_node(const char *name) {
    static const char prefix[] = "chosen";
    for (int i = 0; prefix[i]; i++) {
        if (name[i] != prefix[i]) {
            return 0;
        }
    }
    return name[6] == '\0';
}

size_t fdt_get_memory_regions(const void *dtb_phys, struct fdt_mem_region *out, size_t max_regions) {
    if (read_be32(dtb_phys) != FDT_MAGIC) {
        return 0;
    }

    const uint8_t *base = (const uint8_t *)dtb_phys;
    uint32_t off_dt_struct  = read_be32(base + 8);
    uint32_t off_dt_strings = read_be32(base + 12);
    uint32_t size_dt_struct = read_be32(base + 36);

    const uint8_t *struct_block  = base + off_dt_struct;
    const uint8_t *strings_block = base + off_dt_strings;
    const uint8_t *p   = struct_block;
    const uint8_t *end = struct_block + size_dt_struct;

    size_t count = 0;
    int in_memory_node = 0; // depth of the /memory node we're inside, or 0
    int depth = 0;

    // Devicetree spec defaults; overridden below by the root node's own
    // #address-cells/#size-cells, which is all /memory needs since it's
    // always a direct child of the root.
    uint32_t addr_cells = 2;
    uint32_t size_cells = 1;

    while (p + 4 <= end) {
        uint32_t token = read_be32(p);
        p += 4;

        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            size_t namelen = 0;
            while (p + namelen < end && name[namelen]) {
                namelen++;
            }
            p += align4((uint32_t)namelen + 1);
            depth++;
            if (!in_memory_node && name_is_memory_node(name)) {
                in_memory_node = depth;
            }
        } else if (token == FDT_END_NODE) {
            if (depth == in_memory_node) {
                in_memory_node = 0;
            }
            depth--;
        } else if (token == FDT_PROP) {
            uint32_t len     = read_be32(p);
            uint32_t nameoff = read_be32(p + 4);
            const uint8_t *val = p + 8;
            p += 8 + align4(len);

            const char *pname = (const char *)(strings_block + nameoff);

            if (depth == 1 && len == 4) {
                if (str_eq(pname, "#address-cells")) {
                    addr_cells = read_be32(val);
                } else if (str_eq(pname, "#size-cells")) {
                    size_cells = read_be32(val);
                }
            }

            if (in_memory_node && str_eq(pname, "reg") && addr_cells + size_cells > 0) {
                uint32_t entry_bytes = (addr_cells + size_cells) * 4;
                uint32_t nregions = len / entry_bytes;
                for (uint32_t i = 0; i < nregions && count < max_regions; i++) {
                    const uint8_t *entry = val + i * entry_bytes;
                    out[count].base = read_cells(entry, addr_cells);
                    out[count].size = read_cells(entry + addr_cells * 4, size_cells);
                    count++;
                }
            }
        } else if (token == FDT_NOP) {
            // nothing to do
        } else if (token == FDT_END) {
            break;
        } else {
            // Malformed structure block — bail out rather than run away.
            break;
        }
    }

    return count;
}

int fdt_get_initrd_region(const void *dtb_phys, uint64_t *out_start, uint64_t *out_end) {
    if (read_be32(dtb_phys) != FDT_MAGIC) {
        return 0;
    }

    const uint8_t *base = (const uint8_t *)dtb_phys;
    uint32_t off_dt_struct  = read_be32(base + 8);
    uint32_t off_dt_strings = read_be32(base + 12);
    uint32_t size_dt_struct = read_be32(base + 36);

    const uint8_t *struct_block  = base + off_dt_struct;
    const uint8_t *strings_block = base + off_dt_strings;
    const uint8_t *p   = struct_block;
    const uint8_t *end = struct_block + size_dt_struct;

    int in_chosen_node = 0; // depth of the /chosen node we're inside, or 0
    int depth = 0;
    int have_start = 0, have_end = 0;

    while (p + 4 <= end) {
        uint32_t token = read_be32(p);
        p += 4;

        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            size_t namelen = 0;
            while (p + namelen < end && name[namelen]) {
                namelen++;
            }
            p += align4((uint32_t)namelen + 1);
            depth++;
            if (!in_chosen_node && name_is_chosen_node(name)) {
                in_chosen_node = depth;
            }
        } else if (token == FDT_END_NODE) {
            if (depth == in_chosen_node) {
                in_chosen_node = 0;
            }
            depth--;
        } else if (token == FDT_PROP) {
            uint32_t len     = read_be32(p);
            uint32_t nameoff = read_be32(p + 4);
            const uint8_t *val = p + 8;
            p += 8 + align4(len);

            const char *pname = (const char *)(strings_block + nameoff);

            /* linux,initrd-start/-end aren't governed by any #address-cells
             * (they're not a bus-relative "reg", just raw integers the
             * bootloader writes) — the property's own byte length tells us
             * whether it's a 32-bit or 64-bit cell, same generic-by-length
             * approach as fdt_get_memory_regions() uses for #address-cells. */
            if (in_chosen_node && (len == 4 || len == 8)) {
                if (str_eq(pname, "linux,initrd-start")) {
                    *out_start = read_cells(val, len / 4);
                    have_start = 1;
                } else if (str_eq(pname, "linux,initrd-end")) {
                    *out_end = read_cells(val, len / 4);
                    have_end = 1;
                }
            }
        } else if (token == FDT_NOP) {
            // nothing to do
        } else if (token == FDT_END) {
            break;
        } else {
            break;
        }
    }

    return have_start && have_end;
}
