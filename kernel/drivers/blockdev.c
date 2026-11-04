#include <kernel/blockdev.h>
#include <kernel/klibc/string.h>
#include <kernel/errno.h>

static struct {
    char name[16];
    struct ext2_block_dev *dev;
    bool claimed;
} devs[BLOCKDEV_MAX];

int blockdev_register(const char *name, struct ext2_block_dev *dev) {
    for (unsigned i = 0; i < BLOCKDEV_MAX; i++) {
        if (!devs[i].dev) {
            unsigned n = 0;
            while (name[n] && n < sizeof(devs[i].name) - 1) { devs[i].name[n] = name[n]; n++; }
            devs[i].name[n] = 0;
            devs[i].dev = dev;
            return 0;
        }
    }
    return -ENOSPC;
}

struct ext2_block_dev *blockdev_find(const char *name) {
    if (!name)
        return 0;
    if (!strncmp(name, "/dev/", 5))
        name += 5;
    for (unsigned i = 0; i < BLOCKDEV_MAX; i++)
        if (devs[i].dev && !strcmp(devs[i].name, name))
            return devs[i].dev;
    return 0;
}

const char *blockdev_name_at(unsigned index, struct ext2_block_dev **dev) {
    unsigned seen = 0;
    for (unsigned i = 0; i < BLOCKDEV_MAX; i++) {
        if (!devs[i].dev)
            continue;
        if (seen++ == index) {
            if (dev) *dev = devs[i].dev;
            return devs[i].name;
        }
    }
    return 0;
}

bool blockdev_claim(struct ext2_block_dev *dev) {
    for (unsigned i = 0; i < BLOCKDEV_MAX; i++) {
        if (devs[i].dev == dev) {
            if (devs[i].claimed)
                return false;
            devs[i].claimed = true;
            return true;
        }
    }
    return false;
}

void blockdev_release(struct ext2_block_dev *dev) {
    for (unsigned i = 0; i < BLOCKDEV_MAX; i++)
        if (devs[i].dev == dev)
            devs[i].claimed = false;
}
