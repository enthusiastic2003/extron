/*
 * AF_UNIX sockets: SOCK_STREAM, SOCK_DGRAM and SOCK_SEQPACKET, with
 * socketpair(), filesystem and abstract names, listen/accept/connect,
 * shutdown and sendmsg/recvmsg (no ancillary data yet: SCM_RIGHTS is not
 * implemented).
 *
 * One global lock protects all socket state (single CPU, short critical
 * sections). Sleeping uses sleep()/wakeup() with the socket itself as the
 * "data or state changed" channel and &s->space as the "room for writers"
 * channel. Dropping the last reference to a vfs node can do disk I/O, so
 * that is always done after the lock is released.
 *
 * Ownership: an open file description owns its socket. A connection
 * attempt on a listener creates the server-side socket immediately; it sits
 * in the listener's backlog (owned by it) until accept() hands it to a
 * descriptor. Connected stream sockets point at each other and the survivor
 * is told when one side closes.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <kernel/errno.h>
#include <kernel/fs/file.h>
#include <kernel/fs/socket.h>
#include <kernel/fs/vfs.h>
#include <kernel/klibc/string.h>
#include <kernel/mm/kheap.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/signal.h>
#include <kernel/proc/syscall.h>
#include <kernel/sync/spinlock.h>
#include <arch/irq_spinlock.h>

#define AF_UNIX         1
#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_SEQPACKET  5
#define SOCK_TYPE_MASK  0xF
#define SOCK_NONBLOCK   04000
#define SOCK_CLOEXEC    02000000

#define MSG_PEEK        0x0002
#define MSG_TRUNC       0x0020
#define MSG_DONTWAIT    0x0040
#define MSG_WAITALL     0x0100
#define MSG_NOSIGNAL    0x4000

#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_PASSCRED     16
#define SO_PEERCRED     17
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define SO_ACCEPTCONN   30

#define STREAM_BUF      32768
#define DGRAM_MAX_MSGS  128
#define DGRAM_MAX_BYTES 65536
#define BACKLOG_MAX     16
#define SUN_PATH_MAX    108
#define NAME_ABSTRACT   1

#ifndef O_NONBLOCK
#define O_NONBLOCK 04000
#endif

#ifndef POLLIN
#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLRDHUP 0x2000
#endif

struct dmsg {
    struct dmsg *next;
    size_t len;
    uint8_t from_len;                 /* 0 = sender was unbound */
    uint8_t from_abstract;
    char from[SUN_PATH_MAX];
    uint8_t data[];
};

struct kcred { int32_t pid; uint32_t uid, gid; };

struct usock {
    int type;
    bool bound, listening, connected;
    bool shut_rd, shut_wr, peer_shut_wr, peer_gone;
    bool abstract;
    char name[SUN_PATH_MAX];
    size_t name_len;
    struct vfs_node *node;            /* filesystem name (held), else NULL */
    struct usock *peer;
    struct usock *all_next;           /* every live socket, for name lookup */

    /* stream receive ring */
    uint8_t *buf;
    size_t head, used;
    /* datagram / seqpacket receive queue */
    struct dmsg *dq_head, *dq_tail;
    size_t dq_count, dq_bytes;
    /* listener */
    struct usock *backlog[BACKLOG_MAX];
    int nbacklog, backlog_cap;

    char space;                       /* wait channel: room appeared for writers */
    int pending_error;
    struct kcred peer_cred;
};

static struct usock *all_socks;
static spinlock_t sock_lock = SPINLOCK_INIT;

static inline bool is_stream(const struct usock *s) { return s->type == SOCK_STREAM; }

/* ------------------------------------------------------------------ */
static struct usock *sock_new(int type) {
    struct usock *s = kmalloc(sizeof(*s));
    if (!s)
        return NULL;
    memset(s, 0, sizeof(*s));
    s->type = type;
    if (type == SOCK_STREAM) {
        s->buf = kmalloc(STREAM_BUF);
        if (!s->buf) {
            kfree(s);
            return NULL;
        }
    }
    irq_spin_lock(&sock_lock);
    s->all_next = all_socks;
    all_socks = s;
    irq_spin_unlock(&sock_lock);
    return s;
}

static void creds_of(struct proc *p, int32_t *pid, uint32_t *uid, uint32_t *gid) {
    *pid = (int32_t)p->pid;
    *uid = p->euid;
    *gid = p->egid;
}

static void notify(struct usock *s) {
    wakeup(s);
    file_poll_notify();
}

static void free_dq(struct usock *s) {
    while (s->dq_head) {
        struct dmsg *m = s->dq_head;
        s->dq_head = m->next;
        kfree(m);
    }
    s->dq_tail = NULL;
    s->dq_count = s->dq_bytes = 0;
}

/* Detach `s` from the world. Returns the node whose reference the caller
 * must drop after unlocking (or NULL). Caller holds sock_lock. */
static struct vfs_node *sock_unlink_locked(struct usock *s) {
    struct vfs_node *node = s->node;
    s->node = NULL;
    s->bound = false;
    for (struct usock **pp = &all_socks; *pp; pp = &(*pp)->all_next)
        if (*pp == s) { *pp = s->all_next; break; }

    if (s->peer && s->peer->peer == s) {
        /* Stream/seqpacket peer: it sees EOF / EPIPE from now on. */
        s->peer->peer = NULL;
        s->peer->peer_gone = true;
        notify(s->peer);
        wakeup(&s->peer->space);
    }
    /* Datagram sockets that had `s` as their default destination. */
    for (struct usock *o = all_socks; o; o = o->all_next)
        if (o->peer == s) {
            o->peer = NULL;
            if (!is_stream(o) && o->type == SOCK_DGRAM)
                o->pending_error = ECONNREFUSED;
        }
    /* Pending connections that were never accepted. */
    for (int i = 0; i < s->nbacklog; i++) {
        struct usock *c = s->backlog[i];
        if (c->peer) {
            c->peer->peer = NULL;
            c->peer->peer_gone = true;
            notify(c->peer);
        }
        for (struct usock **pp = &all_socks; *pp; pp = &(*pp)->all_next)
            if (*pp == c) { *pp = c->all_next; break; }
        if (c->buf) kfree(c->buf);
        free_dq(c);
        kfree(c);
    }
    s->nbacklog = 0;
    notify(s);
    wakeup(&s->space);
    return node;
}

void sock_release(struct usock *s) {
    irq_spin_lock(&sock_lock);
    struct vfs_node *node = sock_unlink_locked(s);
    free_dq(s);
    irq_spin_unlock(&sock_lock);
    if (node)
        vfs_node_release(node);
    if (s->buf)
        kfree(s->buf);
    kfree(s);
}

int sock_type(const struct usock *s) { return s->type; }

/* ------------------------------------------------------------------ */
static struct usock *fd_sock(struct proc *p, int fd, int *oflags) {
    return file_socket(p, fd, oflags);
}

static bool name_equal(const struct usock *s, const char *name, size_t len,
                       bool abstract, struct vfs_node *node) {
    if (!s->bound)
        return false;
    if (s->type != SOCK_DGRAM && s->connected)
        return false;                      /* accepted sockets only echo the name */
    if (node)
        return s->node == node;
    return s->abstract == abstract && s->name_len == len && !memcmp(s->name, name, len);
}

static struct usock *lookup_bound(const char *name, size_t len, bool abstract,
                                  struct vfs_node *node, int type) {
    for (struct usock *s = all_socks; s; s = s->all_next)
        if (s->type == type && name_equal(s, name, len, abstract, node))
            return s;
    return NULL;
}

/* Parse a sockaddr_un. On success *abstract/name/len describe the name. */
static long parse_addr(const void *addr, size_t len, bool *abstract,
                       char name[SUN_PATH_MAX + 1], size_t *name_len) {
    if (len < 2 || len > 2 + SUN_PATH_MAX)
        return -EINVAL;
    const uint8_t *a = addr;
    if (a[0] != AF_UNIX || a[1] != 0)
        return -EAFNOSUPPORT;
    size_t plen = len - 2;
    if (plen == 0)
        return -EINVAL;
    if (a[2] == 0) {                       /* abstract namespace */
        *abstract = true;
        memcpy(name, a + 2, plen);
        *name_len = plen;
        return 0;
    }
    *abstract = false;
    size_t n = 0;
    while (n < plen && a[2 + n])
        n++;
    memcpy(name, a + 2, n);
    name[n] = '\0';
    *name_len = n;
    return 0;
}

static uint32_t fill_addr(const struct usock *s, void *out, uint32_t cap) {
    uint8_t tmp[2 + SUN_PATH_MAX + 1];
    memset(tmp, 0, sizeof(tmp));
    tmp[0] = AF_UNIX;
    uint32_t len = 2;
    if (s && s->bound) {
        memcpy(tmp + 2, s->name, s->name_len);
        len = 2 + (uint32_t)s->name_len + (s->abstract ? 0 : 1);
    }
    memcpy(out, tmp, len < cap ? len : cap);
    return len;
}

/* ------------------------------------------------------------------ */
long sock_create(struct proc *p, int domain, int type, int protocol) {
    if (domain != AF_UNIX)
        return -EAFNOSUPPORT;
    if (protocol != 0)
        return -EPROTONOSUPPORT;
    int base = type & SOCK_TYPE_MASK;
    if (base != SOCK_STREAM && base != SOCK_DGRAM && base != SOCK_SEQPACKET)
        return -ESOCKTNOSUPPORT;
    struct usock *s = sock_new(base);
    if (!s)
        return -ENOMEM;
    creds_of(p, &s->peer_cred.pid, &s->peer_cred.uid, &s->peer_cred.gid);
    int fd = file_install_socket(p, s, (type & SOCK_NONBLOCK) ? O_NONBLOCK : 0,
                                 !!(type & SOCK_CLOEXEC));
    if (fd < 0)
        sock_release(s);
    return fd;
}

long sock_pair(struct proc *p, int domain, int type, int protocol, int sv[2]) {
    if (domain != AF_UNIX)
        return -EAFNOSUPPORT;
    if (protocol != 0)
        return -EPROTONOSUPPORT;
    int base = type & SOCK_TYPE_MASK;
    if (base != SOCK_STREAM && base != SOCK_DGRAM && base != SOCK_SEQPACKET)
        return -ESOCKTNOSUPPORT;
    struct usock *a = sock_new(base), *b = sock_new(base);
    if (!a || !b) {
        if (a) sock_release(a);
        if (b) sock_release(b);
        return -ENOMEM;
    }
    irq_spin_lock(&sock_lock);
    a->peer = b; b->peer = a;
    a->connected = b->connected = true;
    creds_of(p, &a->peer_cred.pid, &a->peer_cred.uid, &a->peer_cred.gid);
    b->peer_cred = a->peer_cred;
    irq_spin_unlock(&sock_lock);
    int oflags = (type & SOCK_NONBLOCK) ? O_NONBLOCK : 0;
    int cloexec = !!(type & SOCK_CLOEXEC);
    int fa = file_install_socket(p, a, oflags, cloexec);
    if (fa < 0) { sock_release(a); sock_release(b); return fa; }
    int fb = file_install_socket(p, b, oflags, cloexec);
    if (fb < 0) {
        file_close(p, fa);                 /* releases a */
        sock_release(b);
        return fb;
    }
    sv[0] = fa; sv[1] = fb;
    return 0;
}

long sock_bind(struct proc *p, int fd, const void *addr, size_t len) {
    struct usock *s = fd_sock(p, fd, NULL);
    if (!s)
        return -ENOTSOCK;
    bool abstract; char name[SUN_PATH_MAX + 1]; size_t nlen;
    long r = parse_addr(addr, len, &abstract, name, &nlen);
    if (r < 0)
        return r;
    if (s->bound)
        return -EINVAL;

    struct vfs_node *node = NULL;
    if (!abstract) {
        struct vfs_path cwd;
        if (proc_cwd_snapshot(p, &cwd) < 0)
            return -EIO;
        struct vfs_cred cred;
        proc_vfs_cred_snapshot(p, &cred);
        r = vfs_mknod(&cwd, name, VFS_NODE_SOCKET, 0777 & ~proc_get_umask(p), &cred);
        if (r == 0) {
            struct vfs_path made;
            r = vfs_lookup_path(&cwd, name, &made);
            if (r == 0) {
                node = made.dentry->node;
                vfs_node_retain(node);
                vfs_path_release(&made);
            }
        }
        vfs_path_release(&cwd);
        if (r == -EEXIST)
            return -EADDRINUSE;
        if (r < 0)
            return r;
    }
    irq_spin_lock(&sock_lock);
    if (abstract && lookup_bound(name, nlen, true, NULL, s->type)) {
        irq_spin_unlock(&sock_lock);
        return -EADDRINUSE;
    }
    s->bound = true;
    s->abstract = abstract;
    s->name_len = nlen;
    memcpy(s->name, name, nlen);
    s->node = node;
    irq_spin_unlock(&sock_lock);
    return 0;
}

long sock_listen(struct proc *p, int fd, int backlog) {
    struct usock *s = fd_sock(p, fd, NULL);
    if (!s)
        return -ENOTSOCK;
    if (s->type == SOCK_DGRAM || !s->bound || s->connected)
        return -EOPNOTSUPP;
    irq_spin_lock(&sock_lock);
    s->listening = true;
    s->backlog_cap = backlog < 1 ? 1 : backlog > BACKLOG_MAX ? BACKLOG_MAX : backlog;
    creds_of(p, &s->peer_cred.pid, &s->peer_cred.uid, &s->peer_cred.gid);
    irq_spin_unlock(&sock_lock);
    return 0;
}

long sock_connect(struct proc *p, int fd, const void *addr, size_t len) {
    int oflags;
    struct usock *c = fd_sock(p, fd, &oflags);
    if (!c)
        return -ENOTSOCK;
    bool abstract; char name[SUN_PATH_MAX + 1]; size_t nlen;
    long r = parse_addr(addr, len, &abstract, name, &nlen);
    if (r < 0)
        return r;

    struct vfs_node *node = NULL;
    if (!abstract) {
        struct vfs_path cwd, found;
        if (proc_cwd_snapshot(p, &cwd) < 0)
            return -EIO;
        r = vfs_lookup_path(&cwd, name, &found);
        vfs_path_release(&cwd);
        if (r < 0)
            return r;
        if (found.dentry->node->type != VFS_NODE_SOCKET) {
            vfs_path_release(&found);
            return -ECONNREFUSED;
        }
        node = found.dentry->node;
        vfs_node_retain(node);
        vfs_path_release(&found);
    }

    irq_spin_lock(&sock_lock);
    r = 0;
    struct usock *l = lookup_bound(name, nlen, abstract, node, c->type);
    if (c->type == SOCK_DGRAM) {
        if (!l) r = -ECONNREFUSED;
        else c->peer = l;
        goto out;
    }
    if (c->connected) { r = -EISCONN; goto out; }
    if (c->listening) { r = -EINVAL; goto out; }
    if (!l || !l->listening) { r = -ECONNREFUSED; goto out; }
    if (l->nbacklog >= l->backlog_cap) {
        r = (oflags & O_NONBLOCK) ? -EAGAIN : -ECONNREFUSED;
        goto out;
    }
    struct usock *srv = NULL;
    irq_spin_unlock(&sock_lock);
    srv = sock_new(c->type);
    irq_spin_lock(&sock_lock);
    if (!srv) { r = -ENOMEM; goto out; }
    /* Re-validate: the listener may have closed while we allocated. */
    l = lookup_bound(name, nlen, abstract, node, c->type);
    if (!l || !l->listening || l->nbacklog >= l->backlog_cap) {
        irq_spin_unlock(&sock_lock);
        sock_release(srv);
        irq_spin_lock(&sock_lock);
        r = -ECONNREFUSED;
        goto out;
    }
    srv->connected = c->connected = true;
    srv->peer = c;
    c->peer = srv;
    srv->peer_cred.pid = (int32_t)p->pid;
    srv->peer_cred.uid = p->euid;
    srv->peer_cred.gid = p->egid;
    c->peer_cred = l->peer_cred;
    l->backlog[l->nbacklog++] = srv;
    notify(l);
out:
    irq_spin_unlock(&sock_lock);
    if (node)
        vfs_node_release(node);
    return r;
}

long sock_accept(struct proc *p, int fd, void *addr, uint32_t *addrlen, int flags) {
    int oflags;
    struct usock *l = fd_sock(p, fd, &oflags);
    if (!l)
        return -ENOTSOCK;
    irq_spin_lock(&sock_lock);
    if (!l->listening) {
        irq_spin_unlock(&sock_lock);
        return -EINVAL;
    }
    while (!l->nbacklog) {
        if (oflags & O_NONBLOCK) {
            irq_spin_unlock(&sock_lock);
            return -EAGAIN;
        }
        sleep(l, &sock_lock);
        if (signal_pending_unblocked(my_thread())) {
            irq_spin_unlock(&sock_lock);
            return -4;
        }
    }
    struct usock *srv = l->backlog[0];
    for (int i = 1; i < l->nbacklog; i++)
        l->backlog[i - 1] = l->backlog[i];
    l->nbacklog--;
    /* The accepted socket reports the listener's name, as on Linux. */
    srv->bound = l->bound;
    srv->abstract = l->abstract;
    srv->name_len = l->name_len;
    memcpy(srv->name, l->name, l->name_len);
    irq_spin_unlock(&sock_lock);

    int newfd = file_install_socket(p, srv, (flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0,
                                    !!(flags & SOCK_CLOEXEC));
    if (newfd < 0) {
        sock_release(srv);
        return newfd;
    }
    if (addr && addrlen) {
        struct usock *c = srv->peer;
        uint32_t cap = *addrlen;
        *addrlen = fill_addr(c, addr, cap);
    }
    return newfd;
}

long sock_getname(struct proc *p, int fd, void *addr, uint32_t *addrlen, int peer) {
    struct usock *s = fd_sock(p, fd, NULL);
    if (!s)
        return -ENOTSOCK;
    irq_spin_lock(&sock_lock);
    struct usock *t = s;
    if (peer) {
        t = s->peer;
        if (!t && !(s->connected)) {
            irq_spin_unlock(&sock_lock);
            return -ENOTCONN;
        }
    }
    uint32_t cap = *addrlen;
    *addrlen = fill_addr(t, addr, cap);
    irq_spin_unlock(&sock_lock);
    return 0;
}

long sock_shutdown(struct proc *p, int fd, int how) {
    struct usock *s = fd_sock(p, fd, NULL);
    if (!s)
        return -ENOTSOCK;
    if (how < 0 || how > 2)
        return -EINVAL;
    irq_spin_lock(&sock_lock);
    if (!s->connected) {
        irq_spin_unlock(&sock_lock);
        return -ENOTCONN;
    }
    if (how == 0 || how == 2) {
        s->shut_rd = true;
        notify(s);
        wakeup(&s->space);
    }
    if (how == 1 || how == 2) {
        s->shut_wr = true;
        if (s->peer) {
            s->peer->peer_shut_wr = true;
            notify(s->peer);
        }
    }
    irq_spin_unlock(&sock_lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Copy helpers: iov entries are user memory, re-validated at the moment of
 * the copy because the caller may have slept in between. */
static long gather(struct proc *p, const struct sock_iov *iov, int cnt,
                   size_t skip, void *dst, size_t max) {
    size_t done = 0;
    for (int i = 0; i < cnt && done < max; i++) {
        size_t len = iov[i].len;
        if (skip >= len) { skip -= len; continue; }
        size_t take = len - skip;
        if (take > max - done) take = max - done;
        if (!syscall_user_range_ok(p, (uint64_t)iov[i].base + skip, take))
            return -EFAULT;
        memcpy((uint8_t *)dst + done, (const uint8_t *)iov[i].base + skip, take);
        done += take;
        skip = 0;
    }
    return (long)done;
}

static long scatter(struct proc *p, const struct sock_iov *iov, int cnt,
                    const void *src, size_t n) {
    size_t done = 0;
    for (int i = 0; i < cnt && done < n; i++) {
        size_t take = iov[i].len;
        if (take > n - done) take = n - done;
        if (!take) continue;
        if (!syscall_user_range_ok(p, (uint64_t)iov[i].base, take))
            return -EFAULT;
        memcpy(iov[i].base, (const uint8_t *)src + done, take);
        done += take;
    }
    return (long)done;
}

static size_t iov_total(const struct sock_iov *iov, int cnt) {
    size_t t = 0;
    for (int i = 0; i < cnt; i++) t += iov[i].len;
    return t;
}

static void sigpipe(struct proc *p, int flags) {
    if (!(flags & MSG_NOSIGNAL))
        signal_send(p, 13 /* SIGPIPE */);
}

/* ---- sending ---- */
static long send_stream(struct proc *p, struct usock *s, int oflags,
                        const struct sock_iov *iov, int cnt, int flags) {
    size_t total = iov_total(iov, cnt), sent = 0;
    bool nonblock = (oflags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
    irq_spin_lock(&sock_lock);
    if (!s->connected) {
        irq_spin_unlock(&sock_lock);
        return -ENOTCONN;
    }
    while (sent < total || total == 0) {
        struct usock *d = s->peer;
        if (s->shut_wr || !d || d->shut_rd) {
            irq_spin_unlock(&sock_lock);
            sigpipe(p, flags);
            return sent ? (long)sent : -EPIPE;
        }
        if (total == 0)
            break;
        size_t room = STREAM_BUF - d->used;
        if (!room) {
            if (nonblock) {
                irq_spin_unlock(&sock_lock);
                return sent ? (long)sent : -EAGAIN;
            }
            sleep(&d->space, &sock_lock);
            if (signal_pending_unblocked(my_thread())) {
                irq_spin_unlock(&sock_lock);
                return sent ? (long)sent : -4;
            }
            continue;
        }
        size_t chunk = total - sent;
        if (chunk > room) chunk = room;
        uint8_t tmp[1024];
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
        long g = gather(p, iov, cnt, sent, tmp, chunk);
        if (g < 0) {
            irq_spin_unlock(&sock_lock);
            return sent ? (long)sent : g;
        }
        size_t tail = (d->head + d->used) % STREAM_BUF;
        size_t first = STREAM_BUF - tail;
        if (first > chunk) first = chunk;
        memcpy(d->buf + tail, tmp, first);
        memcpy(d->buf, tmp + first, chunk - first);
        d->used += chunk;
        sent += chunk;
        notify(d);
    }
    irq_spin_unlock(&sock_lock);
    return (long)sent;
}

static long send_dgram(struct proc *p, struct usock *s, int oflags,
                       const void *name, size_t namelen,
                       const struct sock_iov *iov, int cnt, int flags) {
    size_t total = iov_total(iov, cnt);
    if (total > DGRAM_MAX_BYTES)
        return -EMSGSIZE;
    bool nonblock = (oflags & O_NONBLOCK) || (flags & MSG_DONTWAIT);

    struct vfs_node *node = NULL;
    bool abstract = false; char nm[SUN_PATH_MAX + 1]; size_t nlen = 0;
    bool addressed = name && namelen;
    if (addressed) {
        long r = parse_addr(name, namelen, &abstract, nm, &nlen);
        if (r < 0)
            return r;
        if (!abstract) {
            struct vfs_path cwd, found;
            if (proc_cwd_snapshot(p, &cwd) < 0)
                return -EIO;
            r = vfs_lookup_path(&cwd, nm, &found);
            vfs_path_release(&cwd);
            if (r < 0)
                return r;
            if (found.dentry->node->type != VFS_NODE_SOCKET) {
                vfs_path_release(&found);
                return -ECONNREFUSED;
            }
            node = found.dentry->node;
            vfs_node_retain(node);
            vfs_path_release(&found);
        }
    }
    struct dmsg *m = kmalloc(sizeof(*m) + total);
    if (!m) {
        if (node) vfs_node_release(node);
        return -ENOMEM;
    }
    m->next = NULL;
    m->len = total;
    long g = gather(p, iov, cnt, 0, m->data, total);
    if (g < 0) {
        kfree(m);
        if (node) vfs_node_release(node);
        return g;
    }

    long r = 0;
    irq_spin_lock(&sock_lock);
    for (;;) {
        struct usock *d;
        if (addressed) {
            d = lookup_bound(nm, nlen, abstract, node, s->type);
        } else {
            d = s->peer;
            if (!d && !s->connected && s->type != SOCK_DGRAM) { r = -ENOTCONN; break; }
            if (!d) { r = -ENOTCONN; break; }
        }
        if (!d) { r = -ECONNREFUSED; break; }
        if (d->type != SOCK_DGRAM && s->type == SOCK_DGRAM) { r = -ECONNREFUSED; break; }
        if (s->type != SOCK_DGRAM && (s->shut_wr || d->shut_rd)) {
            r = -EPIPE;
            break;
        }
        if (d->dq_count >= DGRAM_MAX_MSGS || d->dq_bytes + total > DGRAM_MAX_BYTES) {
            if (nonblock) { r = -EAGAIN; break; }
            sleep(&d->space, &sock_lock);
            if (signal_pending_unblocked(my_thread())) { r = -4; break; }
            continue;
        }
        m->from_len = 0;
        if (s->bound) {
            m->from_len = (uint8_t)s->name_len;
            m->from_abstract = s->abstract;
            memcpy(m->from, s->name, s->name_len);
        }
        if (d->dq_tail) d->dq_tail->next = m; else d->dq_head = m;
        d->dq_tail = m;
        d->dq_count++;
        d->dq_bytes += total;
        notify(d);
        m = NULL;
        r = (long)total;
        break;
    }
    irq_spin_unlock(&sock_lock);
    if (m) {
        kfree(m);
        if (r == -EPIPE)
            sigpipe(p, flags);
    }
    if (node)
        vfs_node_release(node);
    return r;
}

long sock_sendmsg(struct proc *p, int fd, const void *name, size_t namelen,
                  const struct sock_iov *iov, int cnt, int flags) {
    int oflags;
    struct usock *s = fd_sock(p, fd, &oflags);
    if (!s)
        return -ENOTSOCK;
    if (s->listening)
        return -EINVAL;
    if (is_stream(s)) {
        if (name && namelen)
            return -EISCONN;
        return send_stream(p, s, oflags, iov, cnt, flags);
    }
    return send_dgram(p, s, oflags, name, namelen, iov, cnt, flags);
}

/* ---- receiving ---- */
static long recv_stream(struct proc *p, struct usock *s, int oflags,
                        const struct sock_iov *iov, int cnt, int flags) {
    size_t want = iov_total(iov, cnt), got = 0;
    bool nonblock = (oflags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
    bool peek = flags & MSG_PEEK;
    irq_spin_lock(&sock_lock);
    if (!s->connected && !s->peer_gone) {
        irq_spin_unlock(&sock_lock);
        return -ENOTCONN;
    }
    for (;;) {
        while (!s->used && !s->peer_gone && !s->peer_shut_wr && !s->shut_rd) {
            if ((got && !(flags & MSG_WAITALL)) || nonblock) {
                irq_spin_unlock(&sock_lock);
                return got ? (long)got : -EAGAIN;
            }
            sleep(s, &sock_lock);
            if (signal_pending_unblocked(my_thread())) {
                irq_spin_unlock(&sock_lock);
                return got ? (long)got : -4;
            }
        }
        if (!s->used || want == 0)
            break;                          /* EOF */
        size_t n = s->used;
        if (n > want - got) n = want - got;
        uint8_t tmp[1024];
        if (n > sizeof(tmp)) n = sizeof(tmp);
        size_t first = STREAM_BUF - s->head;
        if (first > n) first = n;
        memcpy(tmp, s->buf + s->head, first);
        memcpy(tmp + first, s->buf, n - first);
        /* Skip the bytes already delivered into earlier iov slots. */
        struct sock_iov rest[8];
        int rc = 0;
        size_t skip = got;
        for (int i = 0; i < cnt && rc < 8; i++) {
            if (skip >= iov[i].len) { skip -= iov[i].len; continue; }
            rest[rc].base = (uint8_t *)iov[i].base + skip;
            rest[rc].len = iov[i].len - skip;
            rc++;
            skip = 0;
        }
        long w = scatter(p, rest, rc, tmp, n);
        if (w < 0) {
            irq_spin_unlock(&sock_lock);
            return got ? (long)got : w;
        }
        if (!peek) {
            s->head = (s->head + (size_t)w) % STREAM_BUF;
            s->used -= (size_t)w;
            wakeup(&s->space);
            file_poll_notify();
        }
        got += (size_t)w;
        if (peek || got >= want)
            break;
        if (!s->used && !(flags & MSG_WAITALL))
            break;
    }
    irq_spin_unlock(&sock_lock);
    return (long)got;
}

static long recv_dgram(struct proc *p, struct usock *s, int oflags,
                       void *name, uint32_t *namelen,
                       const struct sock_iov *iov, int cnt, int flags,
                       int *msg_flags) {
    bool nonblock = (oflags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
    irq_spin_lock(&sock_lock);
    if (s->type == SOCK_SEQPACKET && !s->connected && !s->peer_gone) {
        irq_spin_unlock(&sock_lock);
        return -ENOTCONN;
    }
    while (!s->dq_head) {
        if (s->type == SOCK_SEQPACKET && (s->peer_gone || s->peer_shut_wr || s->shut_rd)) {
            irq_spin_unlock(&sock_lock);
            return 0;
        }
        if (s->pending_error) {
            int e = s->pending_error;
            s->pending_error = 0;
            irq_spin_unlock(&sock_lock);
            return -e;
        }
        if (nonblock) {
            irq_spin_unlock(&sock_lock);
            return -EAGAIN;
        }
        sleep(s, &sock_lock);
        if (signal_pending_unblocked(my_thread())) {
            irq_spin_unlock(&sock_lock);
            return -4;
        }
    }
    struct dmsg *m = s->dq_head;
    size_t cap = iov_total(iov, cnt);
    size_t n = m->len < cap ? m->len : cap;
    long w = scatter(p, iov, cnt, m->data, n);
    if (w < 0) {
        irq_spin_unlock(&sock_lock);
        return w;
    }
    if (name && namelen) {
        uint8_t tmp[2 + SUN_PATH_MAX + 1] = {0};
        tmp[0] = AF_UNIX;
        uint32_t len = 2;
        if (m->from_len) {
            memcpy(tmp + 2, m->from, m->from_len);
            len = 2 + m->from_len + (m->from_abstract ? 0 : 1);
        }
        uint32_t room = *namelen;
        memcpy(name, tmp, len < room ? len : room);
        *namelen = len;
    }
    if (m->len > cap && msg_flags)
        *msg_flags |= MSG_TRUNC;
    long ret = (flags & MSG_TRUNC) ? (long)m->len : w;
    if (!(flags & MSG_PEEK)) {
        s->dq_head = m->next;
        if (!s->dq_head) s->dq_tail = NULL;
        s->dq_count--;
        s->dq_bytes -= m->len;
        kfree(m);
        wakeup(&s->space);
        file_poll_notify();
    }
    irq_spin_unlock(&sock_lock);
    return ret;
}

long sock_recvmsg(struct proc *p, int fd, void *name, uint32_t *namelen,
                  const struct sock_iov *iov, int cnt, int flags, int *msg_flags) {
    int oflags;
    struct usock *s = fd_sock(p, fd, &oflags);
    if (!s)
        return -ENOTSOCK;
    if (s->listening)
        return -EINVAL;
    if (namelen && name == NULL)
        *namelen = 0;
    if (is_stream(s)) {
        if (namelen)
            *namelen = 0;
        return recv_stream(p, s, oflags, iov, cnt, flags);
    }
    return recv_dgram(p, s, oflags, name, namelen, iov, cnt, flags, msg_flags);
}

long sock_read(struct open_file *f, void *buffer, size_t count) {
    struct sock_iov iov = { buffer, count };
    struct proc *p = my_proc();
    struct usock *s = f->object.sock;
    if (is_stream(s))
        return recv_stream(p, s, f->flags, &iov, 1, 0);
    return recv_dgram(p, s, f->flags, NULL, NULL, &iov, 1, 0, NULL);
}

long sock_write(struct open_file *f, const void *buffer, size_t count) {
    struct sock_iov iov = { (void *)buffer, count };
    struct proc *p = my_proc();
    struct usock *s = f->object.sock;
    if (is_stream(s))
        return send_stream(p, s, f->flags, &iov, 1, 0);
    return send_dgram(p, s, f->flags, NULL, 0, &iov, 1, 0);
}

void sock_poll(struct open_file *f, int events, short *revents) {
    struct usock *s = f->object.sock;
    irq_spin_lock(&sock_lock);
    short r = 0;
    if (s->listening) {
        if (s->nbacklog) r |= POLLIN;
    } else if (is_stream(s)) {
        bool eof = s->peer_gone || s->peer_shut_wr || s->shut_rd;
        if (s->used || eof) r |= POLLIN;
        struct usock *d = s->peer;
        if (!s->connected && !s->peer_gone) {
            r |= POLLOUT;
        } else if (!s->shut_wr && d && !d->shut_rd && d->used < STREAM_BUF) {
            r |= POLLOUT;
        }
        if (s->peer_gone) r |= POLLHUP;
        if (eof && (events & POLLRDHUP)) r |= POLLRDHUP;
        if (s->shut_wr && eof) r |= POLLHUP;
    } else {
        if (s->dq_count) r |= POLLIN;
        struct usock *d = s->peer;
        if (!d || (d->dq_count < DGRAM_MAX_MSGS)) r |= POLLOUT;
        if (s->type == SOCK_SEQPACKET && (s->peer_gone || s->peer_shut_wr)) r |= POLLIN | POLLHUP;
        if (s->pending_error) r |= POLLERR;
    }
    irq_spin_unlock(&sock_lock);
    *revents = (short)(r & (events | POLLERR | POLLHUP));
}

long sock_opt(struct proc *p, int fd, int set, int level, int name, void *val, uint32_t *len) {
    struct usock *s = fd_sock(p, fd, NULL);
    if (!s)
        return -ENOTSOCK;
    if (level != SOL_SOCKET)
        return -ENOPROTOOPT;
    if (set) {
        switch (name) {
            case SO_REUSEADDR: case SO_SNDBUF: case SO_RCVBUF: case SO_KEEPALIVE:
            case SO_PASSCRED: case SO_RCVTIMEO: case SO_SNDTIMEO:
                return 0;                  /* accepted, no effect */
            default:
                return -ENOPROTOOPT;
        }
    }
    int ival = 0;
    switch (name) {
        case SO_TYPE:       ival = s->type; break;
        case SO_ERROR:
            irq_spin_lock(&sock_lock);
            ival = s->pending_error;
            s->pending_error = 0;
            irq_spin_unlock(&sock_lock);
            break;
        case SO_ACCEPTCONN: ival = s->listening; break;
        case SO_SNDBUF: case SO_RCVBUF: ival = STREAM_BUF; break;
        case SO_REUSEADDR: case SO_KEEPALIVE: case SO_PASSCRED: ival = 0; break;
        case SO_PEERCRED: {
            if (*len < 12)
                return -EINVAL;
            irq_spin_lock(&sock_lock);
            struct kcred c = s->peer_cred;
            irq_spin_unlock(&sock_lock);
            memcpy(val, &c, 12);
            *len = 12;
            return 0;
        }
        default:
            return -ENOPROTOOPT;
    }
    if (*len >= sizeof(int)) {
        memcpy(val, &ival, sizeof(int));
        *len = sizeof(int);
    }
    return 0;
}

long sock_bytes_available(struct usock *s) {
    irq_spin_lock(&sock_lock);
    long n = is_stream(s) ? (long)s->used : (s->dq_head ? (long)s->dq_head->len : 0);
    irq_spin_unlock(&sock_lock);
    return n;
}
