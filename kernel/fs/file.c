#include <kernel/drivers/pty.h>
#include <kernel/drivers/tty.h>
#include <kernel/fs/file.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/devfs.h>
#include <kernel/errno.h>
#include <kernel/panic.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/mm/kheap.h>
#include <kernel/klibc/string.h>
#include <arch/irq_spinlock.h>
#include <stdbool.h>

/* Extron's mlibc ABI values (abi-bits/fcntl.h). */
#define O_ACCMODE  03
#define O_WRONLY   01
#define O_RDWR     02
#define O_APPEND   02000
#define O_NONBLOCK 04000
#define O_CLOEXEC  02000000
#define FD_CLOEXEC 1

#define POLLIN   0x0001
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010

#define PIPE_CAPACITY 4096

struct pipe_buffer {
    spinlock_t lock;
    size_t head;
    size_t tail;
    size_t used;
    unsigned readers;
    unsigned writers;
    char data[PIPE_CAPACITY];
};

/* Placeholder for file_pipe()'s two-slot reservation below. Never
 * installed as a real descriptor and never dereferenced. */
static struct open_file reserved_slot;
static spinlock_t poll_wait_lock = SPINLOCK_INIT;
static char poll_wait_channel;

void file_poll_notify(void) {
    wakeup(&poll_wait_channel);
}

int file_poll_wait_until(uint64_t deadline_tick) {
    struct thread *t = my_thread();
    irq_spin_lock(&poll_wait_lock);
    t->sleep_until = deadline_tick;
    sleep(&poll_wait_channel, &poll_wait_lock);
    t->sleep_until = 0;
    irq_spin_unlock(&poll_wait_lock);
    return signal_pending_unblocked(t) ? -EINTR : 0;
}

static void file_retain(struct open_file *f) {
    irq_spin_lock(&f->lock);
    f->refs++;
    irq_spin_unlock(&f->lock);
}

static void file_release(struct open_file *f) {
    irq_spin_lock(&f->lock);
    bool last = --f->refs == 0;
    irq_spin_unlock(&f->lock);
    if (!last)
        return;

    if (f->kind == FILE_PIPE_READER || f->kind == FILE_PIPE_WRITER) {
        struct pipe_buffer *pipe = f->object.pipe;
        irq_spin_lock(&pipe->lock);
        if (f->kind == FILE_PIPE_READER)
            pipe->readers--;
        else
            pipe->writers--;
        bool free_pipe = !pipe->readers && !pipe->writers;
        /* Readers need EOF when the final writer closes; writers need to
         * stop sleeping when the final reader disappears. */
        wakeup(pipe);
        file_poll_notify();
        irq_spin_unlock(&pipe->lock);
        if (free_pipe)
            kfree(pipe);
    } else if (f->kind == FILE_VNODE) {
        vfs_path_release(&f->path);
        vfs_node_release(f->object.node);
    }
    kfree(f);
}

static int descriptor_ok(struct proc *p, int fd) {
    return p && fd >= 0 && fd < PROC_MAX_FDS && p->files[fd];
}

static int free_descriptor_from(struct proc *p, int minimum) {
    if (!p || minimum < 0)
        return -1;
    uint64_t limit = resource_nofile_limit(p);
    for (int fd = minimum; fd < (int)limit; fd++)
        if (!p->files[fd])
            return fd;
    return -1;
}

void file_table_init(struct proc *p) {
    for (int fd = 0; fd < PROC_MAX_FDS; fd++) {
        p->files[fd] = NULL;
        p->fd_flags[fd] = 0;
    }

    /* Called from proc_init(), after p->cwd/p->cred are set up (root cwd,
     * uid/gid 0 for every freshly-created proc) but before anything else
     * exists to hand out an fd — so fd 0/1/2 all share one real open-file
     * description against /dev/console, exactly like any other program's
     * stdio would. */
    struct vfs_cred cred;
    struct vfs_path cwd;
    proc_vfs_cred_snapshot(p, &cred);
    proc_cwd_snapshot(p, &cwd);
    struct vfs_node *node;
    struct vfs_path opened;
    int result = vfs_open(&cwd, "/dev/console", O_RDWR, 0, &cred, &node, &opened);
    vfs_path_release(&cwd);
    if (result < 0)
        panic("file_table_init: /dev/console is missing");

    struct open_file *console = kmalloc(sizeof(*console));
    if (!console)
        panic("file_table_init: out of memory");
    memset(console, 0, sizeof(*console));
    console->lock = (spinlock_t)SPINLOCK_INIT;
    console->refs = 3;
    console->kind = FILE_VNODE;
    console->object.node = node;
    console->path = opened;
    console->flags = O_RDWR;
    p->files[0] = p->files[1] = p->files[2] = console;
}

int file_table_clone(struct proc *dst, struct proc *src) {
    if (!dst || !src)
        return -1;
    /* proc_init() installed the default console table. Replace it with an
     * exact snapshot: a closed descriptor must remain closed in the child. */
    for (int fd = 0; fd < PROC_MAX_FDS; fd++) {
        dst->files[fd] = NULL;
        dst->fd_flags[fd] = 0;
    }
    for (int fd = 0; fd < PROC_MAX_FDS; fd++) {
        if (!src->files[fd])
            continue;
        file_retain(src->files[fd]);
        dst->files[fd] = src->files[fd];
        dst->fd_flags[fd] = src->fd_flags[fd];
    }
    return 0;
}

void file_table_close_all(struct proc *p) {
    if (!p)
        return;
    for (int fd = 0; fd < PROC_MAX_FDS; fd++) {
        if (p->files[fd]) {
            struct open_file *f = p->files[fd];
            p->files[fd] = NULL;
            p->fd_flags[fd] = 0;
            file_release(f);
        }
    }
}

void file_table_close_cloexec(struct proc *p) {
    if (!p)
        return;
    for (int fd = 0; fd < PROC_MAX_FDS; fd++)
        if (p->files[fd] && (p->fd_flags[fd] & FD_CLOEXEC))
            file_close(p, fd);
}

int file_open_at(struct proc *p, const struct vfs_path *base,
                 const char *path, int flags, uint32_t mode) {
    if (!p || !base)
        return -EINVAL;
    int fd = free_descriptor_from(p, 0);
    if (fd < 0)
        return -EMFILE;

    struct open_file *f = kmalloc(sizeof(*f));
    if (!f)
        return -ENOMEM;
    memset(f, 0, sizeof(*f));
    struct vfs_node *node;
    struct vfs_cred cred;
    proc_vfs_cred_snapshot(p, &cred);
    mode &= ~proc_get_umask(p);
    int result = vfs_open(base, path, flags, mode, &cred, &node, &f->path);
    if (result < 0) {
        kfree(f);
        return result;
    }
    f->lock = (spinlock_t)SPINLOCK_INIT;
    f->refs = 1;
    f->kind = FILE_VNODE;
    f->object.node = node;
    if (devfs_is_ptmx(f->object.node)) {
        struct vfs_node *master = pty_allocate_master();
        if (!master) {
            vfs_path_release(&f->path);
            vfs_node_release(node);
            kfree(f);
            return -ENOMEM;
        }
        vfs_node_release(f->object.node);
        f->object.node = master;
        node = master; // for vfs_getattr below
    }

    struct vfs_attr attr;
    if (vfs_getattr(node, &attr) != 0) {
        vfs_path_release(&f->path);
        vfs_node_release(node);
        kfree(f);
        return -EIO;
    }
    f->offset = (flags & O_APPEND) ? attr.size : 0;
    f->flags = flags;
    p->files[fd] = f;
    p->fd_flags[fd] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
    return fd;
}

int file_open(struct proc *p, const char *path, int flags, uint32_t mode) {
    if (!p)
        return -EINVAL;
    struct vfs_path cwd;
    if (proc_cwd_snapshot(p, &cwd) < 0)
        return -EIO;
    int result = file_open_at(p, &cwd, path, flags, mode);
    vfs_path_release(&cwd);
    return result;
}

int file_pipe(struct proc *p, int fds[2], int flags) {
    if (!p || !fds || (flags & ~(O_CLOEXEC | O_NONBLOCK)))
        return -EINVAL;
    int read_fd = free_descriptor_from(p, 0);
    if (read_fd < 0)
        return -EMFILE;
    /* Reserve the first slot while finding the second. */
    p->files[read_fd] = &reserved_slot;
    int write_fd = free_descriptor_from(p, 0);
    p->files[read_fd] = NULL;
    if (write_fd < 0)
        return -EMFILE;

    struct pipe_buffer *pipe = kmalloc(sizeof(*pipe));
    struct open_file *reader = kmalloc(sizeof(*reader));
    struct open_file *writer = kmalloc(sizeof(*writer));
    if (!pipe || !reader || !writer) {
        if (pipe) kfree(pipe);
        if (reader) kfree(reader);
        if (writer) kfree(writer);
        return -ENOMEM;
    }
    memset(pipe, 0, sizeof(*pipe));
    memset(reader, 0, sizeof(*reader));
    memset(writer, 0, sizeof(*writer));
    pipe->lock = (spinlock_t)SPINLOCK_INIT;
    pipe->readers = pipe->writers = 1;
    reader->lock = (spinlock_t)SPINLOCK_INIT;
    reader->refs = 1;
    reader->kind = FILE_PIPE_READER;
    reader->object.pipe = pipe;
    reader->flags = flags & O_NONBLOCK;
    writer->lock = (spinlock_t)SPINLOCK_INIT;
    writer->refs = 1;
    writer->kind = FILE_PIPE_WRITER;
    writer->object.pipe = pipe;
    writer->flags = flags & O_NONBLOCK;

    p->files[read_fd] = reader;
    p->files[write_fd] = writer;
    p->fd_flags[read_fd] = p->fd_flags[write_fd] =
        (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
    fds[0] = read_fd;
    fds[1] = write_fd;
    return 0;
}

int file_dup(struct proc *p, int oldfd, int minimum, int cloexec) {
    if (!descriptor_ok(p, oldfd))
        return -EBADF;
    if (minimum < 0 || (uint64_t)minimum >= resource_nofile_limit(p))
        return -EINVAL;
    int newfd = free_descriptor_from(p, minimum);
    if (newfd < 0)
        return -EMFILE;
    file_retain(p->files[oldfd]);
    p->files[newfd] = p->files[oldfd];
    p->fd_flags[newfd] = cloexec ? FD_CLOEXEC : 0;
    return newfd;
}

int file_dup2(struct proc *p, int oldfd, int newfd, int cloexec) {
    if (!descriptor_ok(p, oldfd))
        return -EBADF;
    if (oldfd == newfd)
        return newfd;
    if (newfd < 0 || (uint64_t)newfd >= resource_nofile_limit(p))
        return -EBADF;
    struct open_file *source = p->files[oldfd];
    file_retain(source);
    if (p->files[newfd])
        file_close(p, newfd);
    p->files[newfd] = source;
    p->fd_flags[newfd] = cloexec ? FD_CLOEXEC : 0;
    return newfd;
}

int file_get_fd_flags(struct proc *p, int fd) {
    return descriptor_ok(p, fd) ? p->fd_flags[fd] : -EBADF;
}

int file_set_fd_flags(struct proc *p, int fd, int flags) {
    if (!descriptor_ok(p, fd) || (flags & ~FD_CLOEXEC))
        return !descriptor_ok(p, fd) ? -EBADF : -EINVAL;
    p->fd_flags[fd] = (uint8_t)flags;
    return 0;
}

int file_get_status_flags(struct proc *p, int fd) {
    if (!descriptor_ok(p, fd))
        return -EBADF;
    struct open_file *f = p->files[fd];
    irq_spin_lock(&f->lock);
    int flags = f->flags;
    irq_spin_unlock(&f->lock);
    return flags;
}

int file_set_status_flags(struct proc *p, int fd, int flags) {
    if (!descriptor_ok(p, fd))
        return -EBADF;
    struct open_file *f = p->files[fd];
    irq_spin_lock(&f->lock);
    f->flags = (f->flags & ~(O_APPEND | O_NONBLOCK))
             | (flags & (O_APPEND | O_NONBLOCK));
    irq_spin_unlock(&f->lock);
    return 0;
}

struct extron_dirent {
    uint64_t ino;
    int64_t off;
    uint16_t reclen;
    uint8_t type;
    char name[1024];
};

long file_readdir(struct proc *p, int fd, void *buffer, size_t size) {
    if (!descriptor_ok(p, fd) || size < sizeof(struct extron_dirent))
        return !descriptor_ok(p, fd) ? -EBADF : -EINVAL;
    struct open_file *f = p->files[fd];
    if (f->kind != FILE_VNODE)
        return -ENOTDIR;
    irq_spin_lock(&f->lock);
    struct extron_dirent *entry = buffer;
    struct vfs_dirent ventry;
    int result = vfs_readdir(f->object.node, f->offset, &ventry);
    if (result > 0) {
        entry->ino = ventry.ino;
        entry->off = (int64_t)(f->offset + 1);
        entry->reclen = sizeof(*entry);
        entry->type = ventry.type == VFS_NODE_DIRECTORY ? 4 : 8;
        size_t name_length = strlen(ventry.name) + 1;
        if (name_length > sizeof(entry->name)) {
            irq_spin_unlock(&f->lock);
            return -EIO;
        }
        memcpy(entry->name, ventry.name, name_length);
        f->offset++;
        result = sizeof(*entry);
    }
    irq_spin_unlock(&f->lock);
    return result;
}

int file_info(struct proc *p, int fd, struct vfs_attr *attr) {
    if (!descriptor_ok(p, fd) || !attr)
        return !descriptor_ok(p, fd) ? -EBADF : -EINVAL;
    struct open_file *f = p->files[fd];
    if (f->kind == FILE_VNODE)
        return vfs_getattr(f->object.node, attr);
    memset(attr, 0, sizeof(*attr));
    attr->ino = (uint64_t)fd + 1;
    attr->type = VFS_NODE_REGULAR;
    attr->mode = 0644;
    attr->nlink = 1;
    return 0;
}

int file_get_path(struct proc *p, int fd, struct vfs_path *out) {
    if (!descriptor_ok(p, fd)) return -EBADF;
    struct open_file *f = p->files[fd];
    if (f->kind != FILE_VNODE || !f->path.dentry) return -ENOTDIR;
    if (f->path.dentry->node->type != VFS_NODE_DIRECTORY) return -ENOTDIR;
    *out = f->path;
    vfs_path_retain(out);
    return 0;
}

int file_get_node(struct proc *p, int fd, struct vfs_node **out) {
    if (!descriptor_ok(p, fd) || !out) return -EBADF;
    struct open_file *f = p->files[fd];
    if (f->kind != FILE_VNODE) return -EINVAL;
    *out = f->object.node;
    vfs_node_retain(*out);
    return 0;
}

static long pipe_read(struct open_file *f, void *buffer, size_t count) {
    if (!count)
        return 0;
    struct pipe_buffer *pipe = f->object.pipe;
    irq_spin_lock(&pipe->lock);
    while (!pipe->used && pipe->writers) {
        if (f->flags & O_NONBLOCK) {
            irq_spin_unlock(&pipe->lock);
            return -EAGAIN;
        }
        sleep(pipe, &pipe->lock);
        if (signal_pending_unblocked(my_thread())) {
            irq_spin_unlock(&pipe->lock);
            return -4; /* EINTR */
        }
    }
    if (!pipe->used) {
        irq_spin_unlock(&pipe->lock);
        return 0;
    }
    if (count > pipe->used)
        count = pipe->used;
    size_t first = PIPE_CAPACITY - pipe->tail;
    if (first > count)
        first = count;
    memcpy(buffer, pipe->data + pipe->tail, first);
    memcpy((char *)buffer + first, pipe->data, count - first);
    pipe->tail = (pipe->tail + count) % PIPE_CAPACITY;
    pipe->used -= count;
    wakeup(pipe);
    file_poll_notify();
    irq_spin_unlock(&pipe->lock);
    return (long)count;
}

static long pipe_write(struct open_file *f, const void *buffer, size_t count) {
    struct pipe_buffer *pipe = f->object.pipe;
    size_t written = 0;
    irq_spin_lock(&pipe->lock);

    /* POSIX requires every write no larger than PIPE_BUF to appear as one
     * indivisible record with respect to other writers. Since this pipe's
     * capacity is exactly PIPE_BUF, wait for the complete request to fit
     * before copying any of it. */
    if (count <= PIPE_CAPACITY) {
        while (PIPE_CAPACITY - pipe->used < count && pipe->readers) {
            if (f->flags & O_NONBLOCK) {
                irq_spin_unlock(&pipe->lock);
                return -EAGAIN;
            }
            sleep(pipe, &pipe->lock);
            if (signal_pending_unblocked(my_thread())) {
                irq_spin_unlock(&pipe->lock);
                return -EINTR;
            }
        }
    }

    while (written < count) {
        while (pipe->used == PIPE_CAPACITY && pipe->readers) {
            if (f->flags & O_NONBLOCK) {
                irq_spin_unlock(&pipe->lock);
                return written ? (long)written : -EAGAIN;
            }
            sleep(pipe, &pipe->lock);
            if (signal_pending_unblocked(my_thread())) {
                irq_spin_unlock(&pipe->lock);
                return written ? (long)written : -4; /* EINTR */
            }
        }
        if (!pipe->readers) {
            irq_spin_unlock(&pipe->lock);
            if (!written)
                signal_send(my_proc(), 13 /* SIGPIPE */);
            return written ? (long)written : -EPIPE;
        }
        size_t available = PIPE_CAPACITY - pipe->used;
        size_t chunk = count - written;
        if (chunk > available)
            chunk = available;
        size_t first = PIPE_CAPACITY - pipe->head;
        if (first > chunk)
            first = chunk;
        memcpy(pipe->data + pipe->head, (const char *)buffer + written, first);
        memcpy(pipe->data, (const char *)buffer + written + first, chunk - first);
        pipe->head = (pipe->head + chunk) % PIPE_CAPACITY;
        pipe->used += chunk;
        written += chunk;
        wakeup(pipe);
        file_poll_notify();
    }
    irq_spin_unlock(&pipe->lock);
    return (long)written;
}

long file_read(struct proc *p, int fd, void *buffer, size_t count) {
    if (!descriptor_ok(p, fd))
        return -EBADF;
    struct open_file *f = p->files[fd];
    if (f->kind == FILE_PIPE_READER)
        return pipe_read(f, buffer, count);
    if (f->kind != FILE_VNODE)
        return -EBADF;
    irq_spin_lock(&f->lock);
    if ((f->flags & O_ACCMODE) == O_WRONLY) {
        irq_spin_unlock(&f->lock);
        return -EBADF;
    }
    struct tty *tty = devfs_get_tty(f->object.node);
    long result;
    if (tty) {
        result = tty_read_flags(tty, buffer, count,
                                !!(f->flags & O_NONBLOCK));
    } else if ((f->flags & O_NONBLOCK)
            && pty_get_index(f->object.node) >= 0
            && !pty_master_read_ready(f->object.node)) {
        result = -EAGAIN;
    } else {
        result = vfs_read(f->object.node, f->offset, buffer, count);
    }
    if (result > 0) f->offset += (size_t)result;
    irq_spin_unlock(&f->lock);
    return result;
}

long file_write(struct proc *p, int fd, const void *buffer, size_t count) {
    if (!descriptor_ok(p, fd))
        return -EBADF;
    struct open_file *f = p->files[fd];
    if (f->kind == FILE_PIPE_WRITER)
        return pipe_write(f, buffer, count);
    if (f->kind != FILE_VNODE)
        return -EBADF;
    irq_spin_lock(&f->lock);
    if ((f->flags & O_ACCMODE) == 0) {
        irq_spin_unlock(&f->lock);
        return -EBADF;
    }
    if (f->flags & O_APPEND) {
        struct vfs_attr attr;
        if (vfs_getattr(f->object.node, &attr) != 0) {
            irq_spin_unlock(&f->lock);
            return -EIO;
        }
        f->offset = attr.size;
    }
    struct tty *tty = devfs_get_tty(f->object.node);
    long result;
    if (tty) {
        result = tty_write_flags(tty, buffer, count,
                                 !!(f->flags & O_NONBLOCK));
    } else if (pty_get_index(f->object.node) >= 0) {
        result = pty_master_write_flags(f->object.node, buffer, count,
                                        !!(f->flags & O_NONBLOCK));
    } else {
        result = vfs_write(f->object.node, f->offset, buffer, count);
    }
    if (result > 0) {
        f->offset += (size_t)result;
        struct vfs_cred cred;
        proc_vfs_cred_snapshot(p, &cred);
        if (cred.uid != 0)
            vfs_clear_setid(f->object.node);
    }
    irq_spin_unlock(&f->lock);
    return result;
}

long file_seek(struct proc *p, int fd, int64_t offset, int whence) {
    if (!descriptor_ok(p, fd))
        return -EBADF;
    struct open_file *f = p->files[fd];
    if (f->kind != FILE_VNODE)
        return -ESPIPE;
    irq_spin_lock(&f->lock);
    struct vfs_attr attr;
    if (vfs_getattr(f->object.node, &attr) != 0) {
        irq_spin_unlock(&f->lock);
        return -EIO;
    }
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->offset
                                                    : whence == 2 ? (int64_t)attr.size : -1;
    if (base < 0 || offset < -base) {
        irq_spin_unlock(&f->lock);
        return -EINVAL;
    }
    f->offset = (size_t)(base + offset);
    long result = (long)f->offset;
    irq_spin_unlock(&f->lock);
    return result;
}

int file_close(struct proc *p, int fd) {
    if (!descriptor_ok(p, fd))
        return -EBADF;
    struct open_file *f = p->files[fd];
    p->files[fd] = NULL;
    p->fd_flags[fd] = 0;
    file_release(f);
    return 0;
}

int file_is_tty(struct proc *p, int fd) {
    if (!descriptor_ok(p, fd))
        return 0;
    struct open_file *f = p->files[fd];
    return f->kind == FILE_VNODE && devfs_get_tty(f->object.node) != NULL;
}

struct tty *file_get_tty(struct proc *p, int fd) {
    if (!descriptor_ok(p, fd))
        return NULL;
    struct open_file *f = p->files[fd];
    if (f->kind == FILE_VNODE)
        return devfs_get_tty(f->object.node);
    return NULL;
}

int file_poll(struct proc *p, int fd, short events, short *revents) {
    if (!revents)
        return -EINVAL;
    *revents = 0;
    if (!descriptor_ok(p, fd))
        return -EBADF;
    struct open_file *f = p->files[fd];
    
    struct tty *t = NULL;
    if (f->kind == FILE_VNODE)
        t = devfs_get_tty(f->object.node);
        
    if (t) {
        if ((events & POLLIN) && tty_input_ready(t))
            *revents |= POLLIN;
        if ((events & POLLOUT) && tty_output_ready(t))
            *revents |= POLLOUT;
    } else if (f->kind == FILE_VNODE && pty_get_index(f->object.node) >= 0) {
        if ((events & POLLIN) && pty_master_read_ready(f->object.node))
            *revents |= POLLIN;
        if ((events & POLLOUT) && pty_master_write_ready(f->object.node))
            *revents |= POLLOUT;
    } else if (f->kind == FILE_VNODE) {
        if (events & POLLOUT)
            *revents |= POLLOUT;
        if (events & POLLIN)
            *revents |= POLLIN;
    } else {
        struct pipe_buffer *pipe = f->object.pipe;
        irq_spin_lock(&pipe->lock);
        if (f->kind == FILE_PIPE_READER) {
            if ((events & POLLIN) && (pipe->used || !pipe->writers))
                *revents |= POLLIN;
            if (!pipe->writers)
                *revents |= POLLHUP;
        } else {
            if ((events & POLLOUT) && (pipe->used < PIPE_CAPACITY || !pipe->readers))
                *revents |= POLLOUT;
            if (!pipe->readers)
                *revents |= POLLERR;
        }
        irq_spin_unlock(&pipe->lock);
    }
    return 0;
}
