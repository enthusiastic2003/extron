#ifndef KERNEL_FS_FILE_H
#define KERNEL_FS_FILE_H

#include <stddef.h>
#include <stdint.h>
#include <kernel/sync/spinlock.h>
#include <kernel/fs/vfs.h>

#define PROC_MAX_FDS 32

struct proc;
struct pipe_buffer;
struct usock;

enum open_file_kind {
    FILE_VNODE,
    FILE_PIPE_READER,
    FILE_PIPE_WRITER,
    FILE_PIPE_RDWR,      /* O_RDWR open of a FIFO: both ends of one pipe */
    FILE_SOCKET,         /* AF_UNIX socket (kernel/fs/socket.c) */
    FILE_EPOLL,          /* kernel/fs/evfd.c */
    FILE_EVENTFD,
    FILE_TIMERFD,
    FILE_SIGNALFD,
};

/* One shared open-file description. Descriptor slots are per-process;
 * fork duplicates the slots but retains this object, so parent and child
 * observe the same offset as POSIX requires. */
struct open_file {
    spinlock_t lock;
    unsigned refs;
    enum open_file_kind kind;
    union {
        struct vfs_node *node;
        struct pipe_buffer *pipe;
        struct usock *sock;
        void *special;           /* epoll/eventfd/timerfd/signalfd state */
    } object;
    struct vfs_path path; /* retained location; used by directory-relative *at */
    size_t offset;
    int flags;
};

void file_table_init(struct proc *p);
int  file_table_clone(struct proc *dst, struct proc *src);
void file_table_close_all(struct proc *p);
void file_table_close_cloexec(struct proc *p);

int     file_open(struct proc *p, const char *path, int flags, uint32_t mode);
int     file_open_at(struct proc *p, const struct vfs_path *base,
                     const char *path, int flags, uint32_t mode);
int     file_pipe(struct proc *p, int fds[2], int flags);
int     file_dup(struct proc *p, int oldfd, int minimum, int cloexec);
int     file_dup2(struct proc *p, int oldfd, int newfd, int cloexec);
int     file_get_fd_flags(struct proc *p, int fd);
int     file_set_fd_flags(struct proc *p, int fd, int flags);
int     file_get_status_flags(struct proc *p, int fd);
int     file_set_status_flags(struct proc *p, int fd, int flags);
long    file_read(struct proc *p, int fd, void *buffer, size_t count);
long    file_truncate(struct proc *p, int fd, int64_t size);
long    file_sync(struct proc *p, int fd);
long    file_pread(struct proc *p, int fd, void *buffer, size_t count,
                   int64_t offset);
long    file_pwrite(struct proc *p, int fd, const void *buffer, size_t count,
                    int64_t offset);
long    file_write(struct proc *p, int fd, const void *buffer, size_t count);
long    file_seek(struct proc *p, int fd, int64_t offset, int whence);
int     file_close(struct proc *p, int fd);
long    file_readdir(struct proc *p, int fd, void *buffer, size_t size);
int     file_info(struct proc *p, int fd, struct vfs_attr *attr);
int     file_is_tty(struct proc *p, int fd);
int     file_poll(struct proc *p, int fd, short events, short *revents);
/* All pollable objects publish state changes through one scheduler channel.
 * The current kernel is single-core, so a waiter can scan descriptors and
 * enter this channel without an intervening producer running. */
void    file_poll_notify(void);
uint64_t file_poll_generation(void);
int     file_poll_wait_until(uint64_t deadline_tick);
int     file_get_path(struct proc *p, int fd, struct vfs_path *out);
int     file_get_node(struct proc *p, int fd, struct vfs_node **out);
int     file_fd_is_open(struct proc *p, int fd);
int     file_fd_describe(struct proc *p, int fd, char *out, size_t n);   /* for /proc/<pid>/fd */
/* Sockets: wrap a new socket in a descriptor, or fetch the socket behind one. */
int     file_install_socket(struct proc *p, struct usock *s, int open_flags, int cloexec);
struct usock *file_socket(struct proc *p, int fd, int *open_flags);
/* eventfd/timerfd/signalfd/epoll descriptors (kernel/fs/evfd.c). */
int     file_install_special(struct proc *p, enum open_file_kind kind, void *priv,
                             int open_flags, int cloexec);
void   *file_special(struct open_file *f);
enum open_file_kind file_kind(const struct open_file *f);
int     file_status_flags(const struct open_file *f);
struct open_file *file_description_rw(struct proc *p, int fd);
int     file_pollable(const struct open_file *f);      /* false for regular files/dirs */
void    file_poll_description(struct open_file *f, short events, short *revents);
uint64_t file_poll_next_event_ns(struct open_file *f); /* earliest timerfd expiry or 0 */
const struct open_file *file_description(struct proc *p, int fd);   /* identity token for locks */
uint64_t file_offset(struct proc *p, int fd);

#endif
struct tty *file_get_tty(struct proc *p, int fd);
