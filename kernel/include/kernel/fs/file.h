#ifndef KERNEL_FS_FILE_H
#define KERNEL_FS_FILE_H

#include <stddef.h>
#include <stdint.h>
#include <kernel/sync/spinlock.h>

#define PROC_MAX_FDS 32

struct proc;
struct ramfs_node;

/* One shared open-file description. Descriptor slots are per-process;
 * fork duplicates the slots but retains this object, so parent and child
 * observe the same offset as POSIX requires. */
struct open_file {
    spinlock_t lock;
    unsigned refs;
    struct ramfs_node *node;
    size_t offset;
    int flags;
};

void file_table_init(struct proc *p);
int  file_table_clone(struct proc *dst, struct proc *src);
void file_table_close_all(struct proc *p);

int     file_open(struct proc *p, const char *path, int flags);
long    file_read(struct proc *p, int fd, void *buffer, size_t count);
long    file_write(struct proc *p, int fd, const void *buffer, size_t count);
long    file_seek(struct proc *p, int fd, int64_t offset, int whence);
int     file_close(struct proc *p, int fd);
long    file_readdir(struct proc *p, int fd, void *buffer, size_t size);
int     file_info(struct proc *p, int fd, size_t *size, int *directory);

#endif
