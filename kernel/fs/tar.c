#include <kernel/fs/tar.h>
#include <boot/multiboot2.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/console.h>
#include <kernel/panic.h>
#include <kernel/klibc/string.h>

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
};

static uint64_t tar_start = 0;
static uint64_t tar_end = 0;

// Helper to calculate next multiboot tag
static inline struct multiboot_tag* mb2_next(struct multiboot_tag* tag) {
    return (struct multiboot_tag*)((uint64_t)tag + ((tag->size + 7) & ~7));
}

// Helper to convert octal string to integer
static size_t octal_to_int(const char *str, size_t max_len) {
    size_t val = 0;
    for (size_t i = 0; i < max_len; i++) {
        if (str[i] >= '0' && str[i] <= '7') {
            val = val * 8 + (str[i] - '0');
        } else {
            break;
        }
    }
    return val;
}

// Custom string comparison up to n bytes
static bool tar_streq(const char *a, const char *b, size_t max) {
    for (size_t i = 0; i < max; i++) {
        if (a[i] != b[i]) return false;
        if (a[i] == '\0') return true;
    }
    return true; // Match up to max length
}

void tar_init(uint64_t mb2_addr) {
    // Add HHDM offset to access physical mb2 structure
    uint64_t mb2_virt = mb2_addr + NEW_HDDM;
    
    // First 8 bytes are size and reserved
    struct multiboot_tag *tag = (struct multiboot_tag *)(mb2_virt + 8);
    
    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_MODULE) {
            struct multiboot_tag_module *mod = (struct multiboot_tag_module *)tag;
            
            // Apply HHDM to module start and end
            tar_start = mod->mod_start + NEW_HDDM;
            tar_end = mod->mod_end + NEW_HDDM;
            
            kprintf("TARFS: Found module '%s' at 0x%lx - 0x%lx\n", 
                    mod->cmdline, tar_start, tar_end);
            return; // We only support one module (the initrd) for now
        }
        tag = mb2_next(tag);
    }
    
    kprintf("TARFS: Warning - no multiboot module found!\n");
}

void tar_list(void) {
    if (tar_start == 0 || tar_end == 0) {
        kprintf("TARFS: Not initialized or no module loaded.\n");
        return;
    }

    kprintf("--- TARFS File List ---\n");
    uint64_t ptr = tar_start;
    while (ptr < tar_end) {
        struct tar_header *header = (struct tar_header *)ptr;

        if (header->name[0] == '\0') {
            break; // End of archive
        }

        size_t file_size = octal_to_int(header->size, sizeof(header->size));
        kprintf("File: %s (Size: %zu bytes)\n", header->name, file_size);

        ptr += 512 + align_up(file_size, 512);
    }
    kprintf("-----------------------\n");
}

bool tar_open(const char *name, struct tar_file *out) {
    if (tar_start == 0 || tar_end == 0) return false;

    while (*name == '/') name++;
    while (name[0] == '.' && name[1] == '/') name += 2;

    uint64_t ptr = tar_start;
    while (ptr < tar_end) {
        struct tar_header *header = (struct tar_header *)ptr;

        // Check if we reached the end of the archive (empty filename)
        if (header->name[0] == '\0') {
            break;
        }

        size_t file_size = octal_to_int(header->size, sizeof(header->size));

        if (tar_streq(header->name, name, sizeof(header->name))) {
            out->name = header->name;
            out->size = file_size;
            out->data = (void *)(ptr + 512); // Data follows header
            return true;
        }

        // Advance to next file: 512 bytes for header, then data rounded up to 512 boundary
        ptr += 512 + align_up(file_size, 512);
    }

    return false;
}

void tar_foreach(tar_visit_fn visit, void *ctx) {
    if (!visit || tar_start == 0 || tar_end == 0) return;
    uint64_t ptr = tar_start;
    while (ptr < tar_end) {
        struct tar_header *header = (struct tar_header *)ptr;
        if (!header->name[0]) break;
        size_t size = octal_to_int(header->size, sizeof(header->size));
        struct tar_file file = {
            .name = header->name,
            .size = size,
            .data = (void *)(ptr + 512),
        };
        visit(&file, ctx);
        ptr += 512 + align_up(size, 512);
    }
}
