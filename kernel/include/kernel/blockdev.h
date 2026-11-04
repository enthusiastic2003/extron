#ifndef KERNEL_BLOCKDEV_H
#define KERNEL_BLOCKDEV_H

#include <stdbool.h>
#include <kernel/fs/ext2.h>

/* Registry of block devices (virtio-blk "vda", ...), looked up by name for
 * root= and mount(2). A name may be given as "vda" or "/dev/vda". */
#define BLOCKDEV_MAX 8

int blockdev_register(const char *name, struct ext2_block_dev *dev);
struct ext2_block_dev *blockdev_find(const char *name);
/* Enumerate: returns the name of device `index`, or NULL past the end. */
/* A device holds one filesystem at a time: claim it before mounting (false if
 * already claimed); release on umount. */
bool blockdev_claim(struct ext2_block_dev *dev);
void blockdev_release(struct ext2_block_dev *dev);
const char *blockdev_name_at(unsigned index, struct ext2_block_dev **dev);

#endif
