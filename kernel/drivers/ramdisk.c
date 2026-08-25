#include <kernel/fs/ext2.h>
#include <kernel/fs/tar.h>
#include <kernel/klibc/string.h>
#include <kernel/mm/kheap.h>
#include <kernel/fs/vfs.h>
#include <kernel/console.h>

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

bool try_ramdisk_mount(void) {
    struct tar_file img;
    if (tar_open("ext2.img", &img)) {
        struct ramdisk_dev *rdev = kmalloc(sizeof(*rdev));
        if (!rdev) return false;
        rdev->dev.read_sectors = ram_read;
        rdev->dev.write_sectors = ram_write;
        rdev->dev.sector_size = 512;
        rdev->dev.sector_count = img.size / 512;
        rdev->data = img.data;
        
        struct ext2_mount *ext2_m = ext2_mount_create(&rdev->dev, 0);
        if (ext2_m) {
            extern const struct vfs_fs_ops ext2_fs_ops;
            struct vfs_path root;
            if (vfs_root_path(&root) == 0) {
                int res = vfs_mount_at(&root, "mnt/sd", &ext2_fs_ops, ext2_m);
                if (res == 0) {
                    kprintf("RAMDISK: Successfully mounted ext2.img at /mnt/sd\n");
                    return true;
                }
                kprintf("RAMDISK: vfs_mount_at failed (%d)\n", res);
            }
        } else {
            kprintf("RAMDISK: ext2_mount_create failed!\n");
        }
    } else {
        kprintf("RAMDISK: tar_open failed to find ext2.img\n");
    }
    return false;
}
