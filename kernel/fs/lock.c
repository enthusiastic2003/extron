/*
 * File locking: flock(2) and POSIX record locks.
 *
 * One small global table. A lock names a node (held, so the pointer cannot
 * be recycled while a lock refers to it), an owner, a byte range and a type.
 * flock locks cover the whole file and are owned by the open file
 * description; POSIX locks are owned by the process and cover [start, end).
 * The two families do not conflict with each other, as on Linux.
 *
 * Blocking requests sleep on one channel and re-check whenever any lock is
 * released; a pending signal turns the wait into EINTR. Deadlock detection
 * is not implemented (a cycle of blocked F_SETLKW callers just blocks).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <kernel/errno.h>
#include <kernel/fs/file.h>
#include <kernel/fs/lock.h>
#include <kernel/fs/vfs.h>
#include <kernel/klibc/string.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/signal.h>
#include <kernel/sync/spinlock.h>
#include <arch/irq_spinlock.h>

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

#define F_GETLK  5
#define F_SETLK  6
#define F_SETLKW 7
#define F_RDLCK  0
#define F_WRLCK  1
#define F_UNLCK  2

#define LOCK_TABLE_MAX 256
#define RANGE_END ((uint64_t)INT64_MAX)     /* "to EOF" */

struct flk {
    struct vfs_node *node;      /* NULL = free slot */
    bool posix;                 /* false: flock(2) */
    const void *owner;          /* struct open_file * or struct proc * */
    uint64_t pid;               /* owner's pid, for F_GETLK */
    uint64_t start, end;        /* [start, end) */
    int type;                   /* F_RDLCK / F_WRLCK */
};

static struct flk table[LOCK_TABLE_MAX];
static spinlock_t table_lock = SPINLOCK_INIT;
static char wait_chan;

struct linux_flock {
    int16_t  l_type;
    int16_t  l_whence;
    int32_t  _pad;
    int64_t  l_start;
    int64_t  l_len;
    int32_t  l_pid;
};

static bool overlaps(const struct flk *a, uint64_t s, uint64_t e) {
    return a->start < e && s < a->end;
}

/* Does `want` conflict with some lock held by someone else? */
static struct flk *find_conflict(struct vfs_node *node, bool posix,
                                 const void *owner, uint64_t s, uint64_t e,
                                 int type) {
    for (int i = 0; i < LOCK_TABLE_MAX; i++) {
        struct flk *l = &table[i];
        if (l->node != node || l->posix != posix || l->owner == owner)
            continue;
        if (!overlaps(l, s, e))
            continue;
        if (type == F_WRLCK || l->type == F_WRLCK)
            return l;
    }
    return NULL;
}

static struct flk *alloc_slot(void) {
    for (int i = 0; i < LOCK_TABLE_MAX; i++)
        if (!table[i].node)
            return &table[i];
    return NULL;
}

/* Dropping the last reference to a node can destroy an inode and do disk
 * I/O, which must not happen under the table spinlock. Freed slots park their
 * node here and drain_zombies() releases them after unlocking. */
static struct vfs_node *zombies[LOCK_TABLE_MAX];
static int zombie_count;

static void free_slot(struct flk *l) {
    struct vfs_node *n = l->node;
    l->node = NULL;
    if (n && zombie_count < LOCK_TABLE_MAX)
        zombies[zombie_count++] = n;
}

static void drain_zombies(void) {
    for (;;) {
        struct vfs_node *n = NULL;
        irq_spin_lock(&table_lock);
        if (zombie_count)
            n = zombies[--zombie_count];
        irq_spin_unlock(&table_lock);
        if (!n)
            return;
        vfs_node_release(n);
    }
}

/* Remove [s, e) of every lock `owner` holds on `node`, splitting locks that
 * straddle the range. Returns false when a split needs a slot we lack. */
static bool carve(struct vfs_node *node, bool posix, const void *owner,
                  uint64_t s, uint64_t e) {
    for (int i = 0; i < LOCK_TABLE_MAX; i++) {
        struct flk *l = &table[i];
        if (l->node != node || l->posix != posix || l->owner != owner
                || !overlaps(l, s, e))
            continue;
        if (l->start < s && l->end > e) {            /* split in two */
            struct flk *tail = alloc_slot();
            if (!tail)
                return false;
            *tail = *l;
            vfs_node_retain(node);
            tail->start = e;
            l->end = s;
        } else if (l->start < s) {
            l->end = s;
        } else if (l->end > e) {
            l->start = e;
        } else {
            free_slot(l);
        }
    }
    return true;
}

static int wait_for_unlock(void) {
    /* Called with table_lock held; sleep() drops and retakes it. */
    sleep(&wait_chan, &table_lock);
    return signal_pending_unblocked(my_thread()) ? -4 /* EINTR */ : 0;
}

static void wake_waiters(void) {
    wakeup(&wait_chan);
}

long lock_flock(struct proc *p, int fd, int op) {
    struct vfs_node *node;
    if (file_get_node(p, fd, &node) != 0)
        return -EBADF;                      /* also rejects pipes/ttys */
    const struct open_file *owner = file_description(p, fd);
    bool nonblock = op & LOCK_NB;
    int kind = op & ~LOCK_NB;
    long result = 0;

    irq_spin_lock(&table_lock);
    if (kind == LOCK_UN) {
        carve(node, false, owner, 0, RANGE_END);
        wake_waiters();
        goto out;
    }
    if (kind != LOCK_SH && kind != LOCK_EX) {
        result = -EINVAL;
        goto out;
    }
    int type = kind == LOCK_EX ? F_WRLCK : F_RDLCK;
    /* Converting a lock drops the old one first (not atomic, like Linux). */
    carve(node, false, owner, 0, RANGE_END);
    wake_waiters();
    for (;;) {
        if (!find_conflict(node, false, owner, 0, RANGE_END, type))
            break;
        if (nonblock) { result = -EAGAIN; goto out; }
        int w = wait_for_unlock();
        if (w < 0) { result = w; goto out; }
    }
    struct flk *l = alloc_slot();
    if (!l) { result = -ENOLCK; goto out; }
    l->node = node;
    vfs_node_retain(node);
    l->posix = false;
    l->owner = owner;
    l->pid = p->pid;
    l->start = 0;
    l->end = RANGE_END;
    l->type = type;
out:
    irq_spin_unlock(&table_lock);
    drain_zombies();
    vfs_node_release(node);
    return result;
}

long lock_fcntl(struct proc *p, int fd, int cmd, void *flock_user) {
    struct vfs_node *node;
    if (file_get_node(p, fd, &node) != 0)
        return -EBADF;
    struct linux_flock fl = *(struct linux_flock *)flock_user;
    long result = 0;

    /* Resolve whence-relative start to an absolute range. */
    int64_t base;
    switch (fl.l_whence) {
        case 0: base = 0; break;                             /* SEEK_SET */
        case 1: base = (int64_t)file_offset(p, fd); break;   /* SEEK_CUR */
        case 2: {                                            /* SEEK_END */
            struct vfs_attr a;
            if (vfs_getattr(node, &a) != 0) { vfs_node_release(node); return -EIO; }
            base = (int64_t)a.size;
            break;
        }
        default: vfs_node_release(node); return -EINVAL;
    }
    int64_t start = base + fl.l_start, end;
    if (fl.l_len < 0) {                 /* the range ends just before start */
        end = start;
        start += fl.l_len;
    } else if (fl.l_len == 0) {
        end = (int64_t)RANGE_END;       /* to EOF and beyond */
    } else {
        end = start + fl.l_len;
    }
    if (start < 0 || end < start) { vfs_node_release(node); return -EINVAL; }
    if (fl.l_type != F_RDLCK && fl.l_type != F_WRLCK && fl.l_type != F_UNLCK) {
        vfs_node_release(node);
        return -EINVAL;
    }

    /* A lock needs a descriptor opened for that access. */
    int acc = file_get_status_flags(p, fd) & 3;
    if (cmd != F_GETLK
            && ((fl.l_type == F_RDLCK && acc == 1)
                || (fl.l_type == F_WRLCK && acc == 0))) {
        vfs_node_release(node);
        return -EBADF;
    }

    irq_spin_lock(&table_lock);
    if (cmd == F_GETLK) {
        struct flk *c = fl.l_type == F_UNLCK ? NULL
            : find_conflict(node, true, p, (uint64_t)start, (uint64_t)end,
                            fl.l_type);
        if (!c) {
            fl.l_type = F_UNLCK;
        } else {
            fl.l_type = (int16_t)c->type;
            fl.l_whence = 0;
            fl.l_start = (int64_t)c->start;
            fl.l_len = c->end == RANGE_END ? 0 : (int64_t)(c->end - c->start);
            fl.l_pid = (int32_t)c->pid;
        }
        *(struct linux_flock *)flock_user = fl;
        goto out;
    }
    if (fl.l_type == F_UNLCK) {
        if (!carve(node, true, p, (uint64_t)start, (uint64_t)end))
            result = -ENOLCK;
        wake_waiters();
        goto out;
    }
    for (;;) {
        if (!find_conflict(node, true, p, (uint64_t)start, (uint64_t)end,
                           fl.l_type))
            break;
        if (cmd == F_SETLK) { result = -EAGAIN; goto out; }
        int w = wait_for_unlock();
        if (w < 0) { result = w; goto out; }
    }
    /* Replace our own overlapping locks with the new one. */
    if (!carve(node, true, p, (uint64_t)start, (uint64_t)end)) {
        result = -ENOLCK;
        goto out;
    }
    struct flk *l = alloc_slot();
    if (!l) { result = -ENOLCK; goto out; }
    l->node = node;
    vfs_node_retain(node);
    l->posix = true;
    l->owner = p;
    l->pid = p->pid;
    l->start = (uint64_t)start;
    l->end = (uint64_t)end;
    l->type = fl.l_type;
    wake_waiters();                     /* a downgrade can unblock others */
out:
    irq_spin_unlock(&table_lock);
    drain_zombies();
    vfs_node_release(node);
    return result;
}

void lock_release_file(struct open_file *f) {
    bool any = false;
    irq_spin_lock(&table_lock);
    for (int i = 0; i < LOCK_TABLE_MAX; i++)
        if (table[i].node && !table[i].posix && table[i].owner == f) {
            free_slot(&table[i]);
            any = true;
        }
    if (any)
        wake_waiters();
    irq_spin_unlock(&table_lock);
    drain_zombies();
}

void lock_release_proc_node(struct proc *p, struct vfs_node *node) {
    bool any = false;
    irq_spin_lock(&table_lock);
    for (int i = 0; i < LOCK_TABLE_MAX; i++)
        if (table[i].node == node && table[i].posix && table[i].owner == p) {
            free_slot(&table[i]);
            any = true;
        }
    if (any)
        wake_waiters();
    irq_spin_unlock(&table_lock);
    drain_zombies();
}
