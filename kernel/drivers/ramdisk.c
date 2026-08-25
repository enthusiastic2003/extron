#include <kernel/klibc/string.h>
#include <kernel/fs/ext2.h>
#include <kernel/mm/kheap.h>
#include <kernel/fs/vfs.h>
#include <kernel/console.h>
#include <boot/multiboot2.h>

struct ramdisk_dev {
    struct ext2_block_dev dev;
    uint8_t *data;
};

static int ram_read(struct ext2_block_dev *dev, uint64_t lba, size_t count, void *buf) {
    struct ramdisk_dev *r = (struct ramdisk_dev *)dev;
    if (lba + count > dev->sector_count) return -1;
    memcpy(buf, r->data + (lba * dev->sector_size), count * dev->sector_size);
    return 0;
}

static int ram_write(struct ext2_block_dev *dev, uint64_t lba, size_t count, const void *buf) {
    struct ramdisk_dev *r = (struct ramdisk_dev *)dev;
    if (lba + count > dev->sector_count) return -1;
    memcpy(r->data + (lba * dev->sector_size), buf, count * dev->sector_size);
    return 0;
}

static inline struct multiboot_tag* mb2_next(struct multiboot_tag* tag) {
    return (struct multiboot_tag*)((uint64_t)tag + ((tag->size + 7) & ~7));
}

bool try_ramdisk_mount(uint64_t mb2_addr) {
    uint8_t *initrd_data = NULL;
    uint32_t initrd_size = 0;

    // The multiboot tags start 8 bytes after the mb2_addr
    struct multiboot_tag* tag = (struct multiboot_tag*)(mb2_addr + 8);
    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_MODULE) {
            struct multiboot_tag_module* mod = (struct multiboot_tag_module*)tag;
            // Add HHDM offset to access physical mb2 structure
            initrd_data = (uint8_t *)(mod->mod_start + NEW_HDDM);
            initrd_size = mod->mod_end - mod->mod_start;
            break;
        }
        tag = mb2_next(tag);
    }

    if (initrd_data && initrd_size > 0) {
        struct ramdisk_dev *rdev = kmalloc(sizeof(*rdev));
        if (!rdev) return false;
        rdev->dev.read_sectors = ram_read;
        rdev->dev.write_sectors = ram_write;
        rdev->dev.sector_size = 512;
        rdev->dev.sector_count = initrd_size / 512;
        rdev->data = initrd_data;
        
        struct ext2_mount *ext2_m = ext2_mount_create(&rdev->dev, 0);
        if (ext2_m) {
            extern const struct vfs_fs_ops ext2_fs_ops;
            int res = vfs_mount_root(&ext2_fs_ops, ext2_m);
            if (res == 0) {
                kprintf("RAMDISK: Successfully mounted Ext2 initrd as root (/)!\n");
                return true;
            }
            kprintf("RAMDISK: vfs_mount_root failed (%d)\n", res);
        } else {
            kprintf("RAMDISK: ext2_mount_create failed! (Not a valid ext2 image)\n");
        }
    } else {
        kprintf("RAMDISK: No initrd module found in mb2_addr\n");
    }
    return false;
}
