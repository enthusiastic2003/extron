#include <kernel/fs/file.h>
#include <kernel/fs/ramfs.h>
#include <kernel/proc/proc.h>
#include <kernel/mm/kheap.h>
#include <kernel/klibc/string.h>
#include <arch/irq_spinlock.h>

/* Extron's mlibc ABI values (abi-bits/fcntl.h). */
#define O_ACCMODE 03
#define O_WRONLY  01
#define O_RDWR    02
#define O_APPEND  02000

void file_table_init(struct proc *p) {
    for (int fd = 0; fd < PROC_MAX_FDS; fd++)
        p->files[fd] = NULL;
}

static void file_retain(struct open_file *f) {
    irq_spin_lock(&f->lock);
    f->refs++;
    irq_spin_unlock(&f->lock);
}

static void file_release(struct open_file *f) {
    bool last;
    irq_spin_lock(&f->lock);
    last = --f->refs == 0;
    irq_spin_unlock(&f->lock);
    if (last)
        kfree(f);
}

int file_table_clone(struct proc *dst, struct proc *src) {
    file_table_init(dst);
    for (int fd = 3; fd < PROC_MAX_FDS; fd++) {
        if (!src->files[fd])
            continue;
        file_retain(src->files[fd]);
        dst->files[fd] = src->files[fd];
    }
    return 0;
}

void file_table_close_all(struct proc *p) {
    for (int fd = 3; fd < PROC_MAX_FDS; fd++) {
        if (p->files[fd]) {
            file_release(p->files[fd]);
            p->files[fd] = NULL;
        }
    }
}

int file_open(struct proc *p, const char *path, int flags) {
    if (!p) return -1;

    int fd;
    for (fd = 3; fd < PROC_MAX_FDS; fd++)
        if (!p->files[fd])
            break;
    if (fd == PROC_MAX_FDS)
        return -1;

    struct open_file *f = kmalloc(sizeof(*f));
    if (!f)
        return -1;
    struct ramfs_node *node;
    if (ramfs_open(path, flags, &node) != 0) {
        kfree(f);
        return -1;
    }
    f->lock = (spinlock_t)SPINLOCK_INIT;
    f->refs = 1;
    f->node = node;
    f->offset = (flags & O_APPEND) ? ramfs_size(node) : 0;
    f->flags = flags;
    p->files[fd] = f;
    return fd;
}

long file_read(struct proc *p, int fd, void *buffer, size_t count) {
    if (!p || fd < 3 || fd >= PROC_MAX_FDS || !p->files[fd])
        return -1;
    struct open_file *f = p->files[fd];
    irq_spin_lock(&f->lock);
    if ((f->flags & O_ACCMODE) == O_WRONLY) {
        irq_spin_unlock(&f->lock);
        return -1;
    }
    long result = ramfs_read(f->node, f->offset, buffer, count);
    if (result > 0) f->offset += (size_t)result;
    irq_spin_unlock(&f->lock);
    return result;
}

long file_write(struct proc *p, int fd, const void *buffer, size_t count) {
    if (!p || fd < 3 || fd >= PROC_MAX_FDS || !p->files[fd]) return -1;
    struct open_file *f = p->files[fd];
    irq_spin_lock(&f->lock);
    if ((f->flags & O_ACCMODE) == 0) { irq_spin_unlock(&f->lock); return -1; }
    if (f->flags & O_APPEND) f->offset = ramfs_size(f->node);
    long result = ramfs_write(f->node, f->offset, buffer, count);
    if (result > 0) f->offset += (size_t)result;
    irq_spin_unlock(&f->lock);
    return result;
}

long file_seek(struct proc *p, int fd, int64_t offset, int whence) {
    if (!p || fd < 3 || fd >= PROC_MAX_FDS || !p->files[fd])
        return -1;
    struct open_file *f = p->files[fd];
    irq_spin_lock(&f->lock);
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->offset
                                                    : whence == 2 ? (int64_t)ramfs_size(f->node) : -1;
    if (base < 0 || offset < -base) {
        irq_spin_unlock(&f->lock);
        return -1;
    }
    f->offset = (size_t)(base + offset);
    long result = (long)f->offset;
    irq_spin_unlock(&f->lock);
    return result;
}

int file_close(struct proc *p, int fd) {
    if (!p || fd < 3 || fd >= PROC_MAX_FDS || !p->files[fd])
        return -1;
    struct open_file *f = p->files[fd];
    p->files[fd] = NULL;
    file_release(f);
    return 0;
}
