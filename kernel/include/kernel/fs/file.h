#ifndef KERNEL_FS_FILE_H
#define KERNEL_FS_FILE_H

#include <stddef.h>
#include <stdint.h>
#include <kernel/sync/spinlock.h>

#define PROC_MAX_FDS 32

struct proc;
struct ramfs_node;
struct pipe_buffer;

enum open_file_kind {
    FILE_CONSOLE_IN,
    FILE_CONSOLE_OUT,
    FILE_RAMFS,
    FILE_PIPE_READER,
    FILE_PIPE_WRITER,
};

/* One shared open-file description. Descriptor slots are per-process;
 * fork duplicates the slots but retains this object, so parent and child
 * observe the same offset as POSIX requires. */
struct open_file {
    spinlock_t lock;
    unsigned refs;
    enum open_file_kind kind;
    union {
        struct ramfs_node *node;
        struct pipe_buffer *pipe;
    } object;
    size_t offset;
    int flags;
};

void file_table_init(struct proc *p);
int  file_table_clone(struct proc *dst, struct proc *src);
void file_table_close_all(struct proc *p);
void file_table_close_cloexec(struct proc *p);

int     file_open(struct proc *p, const char *path, int flags);
int     file_pipe(struct proc *p, int fds[2], int flags);
int     file_dup(struct proc *p, int oldfd, int minimum, int cloexec);
int     file_dup2(struct proc *p, int oldfd, int newfd, int cloexec);
int     file_get_fd_flags(struct proc *p, int fd);
int     file_set_fd_flags(struct proc *p, int fd, int flags);
int     file_get_status_flags(struct proc *p, int fd);
int     file_set_status_flags(struct proc *p, int fd, int flags);
long    file_read(struct proc *p, int fd, void *buffer, size_t count);
long    file_write(struct proc *p, int fd, const void *buffer, size_t count);
long    file_seek(struct proc *p, int fd, int64_t offset, int whence);
int     file_close(struct proc *p, int fd);
long    file_readdir(struct proc *p, int fd, void *buffer, size_t size);
int     file_info(struct proc *p, int fd, size_t *size, int *directory);
int     file_is_tty(struct proc *p, int fd);
int     file_poll(struct proc *p, int fd, short events, short *revents);

#endif
