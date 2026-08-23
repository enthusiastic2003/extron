#ifndef KERNEL_FS_RAMFS_H
#define KERNEL_FS_RAMFS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <kernel/sync/spinlock.h>

struct ramfs_node {
    spinlock_t lock;
    char path[101];
    bool directory;
    bool owns_data;
    uint8_t *data;
    size_t size;
    size_t capacity;
    struct ramfs_node *next;
};

void ramfs_init(void);
int ramfs_open(const char *path, int flags, struct ramfs_node **out);
int ramfs_mkdir(const char *path);
long ramfs_read(struct ramfs_node *node, size_t offset, void *buffer, size_t count);
long ramfs_write(struct ramfs_node *node, size_t offset, const void *buffer, size_t count);
size_t ramfs_size(struct ramfs_node *node);

#endif
