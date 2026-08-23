#include <kernel/fs/file.h>
#include <kernel/fs/vfs.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <kernel/drivers/tty.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/mm/kheap.h>
#include <kernel/klibc/string.h>
#include <arch/irq_spinlock.h>
#include <stdbool.h>

/* Extron's mlibc ABI values (abi-bits/fcntl.h). */
#define O_ACCMODE  03
#define O_WRONLY   01
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

/* Console descriptions are permanent kernel objects. Descriptor slots can
 * refer to them, duplicate them, or close them, but they are never freed. */
static struct open_file console_input = {
    .lock = SPINLOCK_INIT,
    .refs = 1,
    .kind = FILE_CONSOLE_IN,
};
static struct open_file console_output = {
    .lock = SPINLOCK_INIT,
    .refs = 1,
    .kind = FILE_CONSOLE_OUT,
};

static bool permanent_file(const struct open_file *f) {
    return f == &console_input || f == &console_output;
}

static void file_retain(struct open_file *f) {
    if (permanent_file(f))
        return;
    irq_spin_lock(&f->lock);
    f->refs++;
    irq_spin_unlock(&f->lock);
}

static void file_release(struct open_file *f) {
    if (permanent_file(f))
        return;

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
        irq_spin_unlock(&pipe->lock);
        if (free_pipe)
            kfree(pipe);
    }
    kfree(f);
}

static int descriptor_ok(struct proc *p, int fd) {
    return p && fd >= 0 && fd < PROC_MAX_FDS && p->files[fd];
}

static int free_descriptor_from(struct proc *p, int minimum) {
    if (!p || minimum < 0)
        return -1;
    for (int fd = minimum; fd < PROC_MAX_FDS; fd++)
        if (!p->files[fd])
            return fd;
    return -1;
}

void file_table_init(struct proc *p) {
    for (int fd = 0; fd < PROC_MAX_FDS; fd++) {
        p->files[fd] = NULL;
        p->fd_flags[fd] = 0;
    }
    p->files[0] = &console_input;
    p->files[1] = &console_output;
    p->files[2] = &console_output;
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

int file_open(struct proc *p, const char *path, int flags) {
    if (!p)
        return -1;
    int fd = free_descriptor_from(p, 0);
    if (fd < 0)
        return -1;

    struct open_file *f = kmalloc(sizeof(*f));
    if (!f)
        return -1;
    struct vfs_node *node;
    if (vfs_open(p->cwd, path, flags, &node) != 0) {
        kfree(f);
        return -1;
    }
    memset(f, 0, sizeof(*f));
    f->lock = (spinlock_t)SPINLOCK_INIT;
    f->refs = 1;
    f->kind = FILE_VNODE;
    f->object.node = node;
    struct vfs_attr attr;
    if (vfs_getattr(node, &attr) != 0) {
        kfree(f);
        return -1;
    }
    f->offset = (flags & O_APPEND) ? attr.size : 0;
    f->flags = flags;
    p->files[fd] = f;
    p->fd_flags[fd] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
    return fd;
}

int file_pipe(struct proc *p, int fds[2], int flags) {
    if (!p || !fds || (flags & ~O_CLOEXEC))
        return -1;
    int read_fd = free_descriptor_from(p, 0);
    if (read_fd < 0)
        return -1;
    /* Reserve the first slot while finding the second. */
    p->files[read_fd] = &console_input;
    int write_fd = free_descriptor_from(p, 0);
    p->files[read_fd] = NULL;
    if (write_fd < 0)
        return -1;

    struct pipe_buffer *pipe = kmalloc(sizeof(*pipe));
    struct open_file *reader = kmalloc(sizeof(*reader));
    struct open_file *writer = kmalloc(sizeof(*writer));
    if (!pipe || !reader || !writer) {
        if (pipe) kfree(pipe);
        if (reader) kfree(reader);
        if (writer) kfree(writer);
        return -1;
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
    writer->lock = (spinlock_t)SPINLOCK_INIT;
    writer->refs = 1;
    writer->kind = FILE_PIPE_WRITER;
    writer->object.pipe = pipe;

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
        return -1;
    int newfd = free_descriptor_from(p, minimum);
    if (newfd < 0)
        return -1;
    file_retain(p->files[oldfd]);
    p->files[newfd] = p->files[oldfd];
    p->fd_flags[newfd] = cloexec ? FD_CLOEXEC : 0;
    return newfd;
}

int file_dup2(struct proc *p, int oldfd, int newfd, int cloexec) {
    if (!descriptor_ok(p, oldfd) || newfd < 0 || newfd >= PROC_MAX_FDS)
        return -1;
    if (oldfd == newfd)
        return newfd;
    struct open_file *source = p->files[oldfd];
    file_retain(source);
    if (p->files[newfd])
        file_close(p, newfd);
    p->files[newfd] = source;
    p->fd_flags[newfd] = cloexec ? FD_CLOEXEC : 0;
    return newfd;
}

int file_get_fd_flags(struct proc *p, int fd) {
    return descriptor_ok(p, fd) ? p->fd_flags[fd] : -1;
}

int file_set_fd_flags(struct proc *p, int fd, int flags) {
    if (!descriptor_ok(p, fd) || (flags & ~FD_CLOEXEC))
        return -1;
    p->fd_flags[fd] = (uint8_t)flags;
    return 0;
}

int file_get_status_flags(struct proc *p, int fd) {
    if (!descriptor_ok(p, fd))
        return -1;
    struct open_file *f = p->files[fd];
    irq_spin_lock(&f->lock);
    int flags = f->flags;
    irq_spin_unlock(&f->lock);
    return flags;
}

int file_set_status_flags(struct proc *p, int fd, int flags) {
    if (!descriptor_ok(p, fd))
        return -1;
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
        return -1;
    struct open_file *f = p->files[fd];
    if (f->kind != FILE_VNODE)
        return -1;
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
            return -1;
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
        return -1;
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

static long pipe_read(struct open_file *f, void *buffer, size_t count) {
    if (!count)
        return 0;
    struct pipe_buffer *pipe = f->object.pipe;
    irq_spin_lock(&pipe->lock);
    while (!pipe->used && pipe->writers) {
        if (f->flags & O_NONBLOCK) {
            irq_spin_unlock(&pipe->lock);
            return -1;
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
    irq_spin_unlock(&pipe->lock);
    return (long)count;
}

static long pipe_write(struct open_file *f, const void *buffer, size_t count) {
    struct pipe_buffer *pipe = f->object.pipe;
    size_t written = 0;
    irq_spin_lock(&pipe->lock);
    while (written < count) {
        while (pipe->used == PIPE_CAPACITY && pipe->readers) {
            if (f->flags & O_NONBLOCK) {
                irq_spin_unlock(&pipe->lock);
                return written ? (long)written : -1;
            }
            sleep(pipe, &pipe->lock);
            if (signal_pending_unblocked(my_thread())) {
                irq_spin_unlock(&pipe->lock);
                return written ? (long)written : -4; /* EINTR */
            }
        }
        if (!pipe->readers) {
            irq_spin_unlock(&pipe->lock);
            return written ? (long)written : -1;
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
    }
    irq_spin_unlock(&pipe->lock);
    return (long)written;
}

long file_read(struct proc *p, int fd, void *buffer, size_t count) {
    if (!descriptor_ok(p, fd))
        return -1;
    struct open_file *f = p->files[fd];
    if (f->kind == FILE_CONSOLE_IN)
        return tty_read(buffer, count);
    if (f->kind == FILE_PIPE_READER)
        return pipe_read(f, buffer, count);
    if (f->kind != FILE_VNODE)
        return -1;
    irq_spin_lock(&f->lock);
    if ((f->flags & O_ACCMODE) == O_WRONLY) {
        irq_spin_unlock(&f->lock);
        return -1;
    }
    long result = vfs_read(f->object.node, f->offset, buffer, count);
    if (result > 0) f->offset += (size_t)result;
    irq_spin_unlock(&f->lock);
    return result;
}

long file_write(struct proc *p, int fd, const void *buffer, size_t count) {
    if (!descriptor_ok(p, fd))
        return -1;
    struct open_file *f = p->files[fd];
    if (f->kind == FILE_CONSOLE_OUT)
        return tty_write(buffer, count);
    if (f->kind == FILE_PIPE_WRITER)
        return pipe_write(f, buffer, count);
    if (f->kind != FILE_VNODE)
        return -1;
    irq_spin_lock(&f->lock);
    if ((f->flags & O_ACCMODE) == 0) {
        irq_spin_unlock(&f->lock);
        return -1;
    }
    if (f->flags & O_APPEND) {
        struct vfs_attr attr;
        if (vfs_getattr(f->object.node, &attr) != 0) {
            irq_spin_unlock(&f->lock);
            return -1;
        }
        f->offset = attr.size;
    }
    long result = vfs_write(f->object.node, f->offset, buffer, count);
    if (result > 0) f->offset += (size_t)result;
    irq_spin_unlock(&f->lock);
    return result;
}

long file_seek(struct proc *p, int fd, int64_t offset, int whence) {
    if (!descriptor_ok(p, fd))
        return -1;
    struct open_file *f = p->files[fd];
    if (f->kind != FILE_VNODE)
        return -1;
    irq_spin_lock(&f->lock);
    struct vfs_attr attr;
    if (vfs_getattr(f->object.node, &attr) != 0) {
        irq_spin_unlock(&f->lock);
        return -1;
    }
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->offset
                                                    : whence == 2 ? (int64_t)attr.size : -1;
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
    if (!descriptor_ok(p, fd))
        return -1;
    struct open_file *f = p->files[fd];
    p->files[fd] = NULL;
    p->fd_flags[fd] = 0;
    file_release(f);
    return 0;
}

int file_is_tty(struct proc *p, int fd) {
    if (!descriptor_ok(p, fd))
        return 0;
    enum open_file_kind kind = p->files[fd]->kind;
    return kind == FILE_CONSOLE_IN || kind == FILE_CONSOLE_OUT;
}

int file_poll(struct proc *p, int fd, short events, short *revents) {
    if (!revents)
        return -1;
    *revents = 0;
    if (!descriptor_ok(p, fd))
        return -1;
    struct open_file *f = p->files[fd];
    if (f->kind == FILE_CONSOLE_IN) {
        if ((events & POLLIN) && kbd_input_ready())
            *revents |= POLLIN;
    } else if (f->kind == FILE_CONSOLE_OUT || f->kind == FILE_VNODE) {
        if (events & POLLOUT)
            *revents |= POLLOUT;
        if (f->kind == FILE_VNODE && (events & POLLIN))
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
            if ((events & POLLOUT) && pipe->used < PIPE_CAPACITY && pipe->readers)
                *revents |= POLLOUT;
            if (!pipe->readers)
                *revents |= POLLERR;
        }
        irq_spin_unlock(&pipe->lock);
    }
    return *revents != 0;
}
