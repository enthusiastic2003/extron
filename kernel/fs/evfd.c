/*
 * epoll, eventfd, timerfd and signalfd.
 *
 * All of this state is touched only from syscall context, which on this
 * kernel runs with interrupts masked and is not preempted (the same property
 * the pipe and socket code lean on), so there is no separate lock: loops that
 * sleep re-scan after waking and never hold a pointer across the sleep.
 *
 * Waiting reuses the poll() machinery: every state change calls
 * file_poll_notify(), and sleepers wait with file_poll_wait_until(). A timerfd
 * has no timer interrupt of its own; instead each waiter folds the earliest
 * pending expiry into its sleep deadline (evfd_next_event_ns), and the
 * expiration count is computed lazily from the clock.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <kernel/drivers/timer.h>
#include <kernel/errno.h>
#include <kernel/fs/evfd.h>
#include <kernel/fs/file.h>
#include <kernel/klibc/string.h>
#include <kernel/mm/kheap.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/signal.h>

#define O_NONBLOCK_ 04000
#define O_CLOEXEC_  02000000

#define POLLIN_   0x001
#define POLLOUT_  0x004
#define POLLERR_  0x008
#define POLLHUP_  0x010

#define EPOLLET        (1u << 31)
#define EPOLLONESHOT   (1u << 30)
#define EPOLLEXCLUSIVE (1u << 28)
#define EPOLLWAKEUP    (1u << 29)
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EFD_SEMAPHORE 1
#define TFD_TIMER_ABSTIME 1
#define CLOCK_REALTIME_ 0
#define CLOCK_MONOTONIC_ 1

/* ------------------------------------------------------------------ */
/* eventfd */
struct eventfd { uint64_t counter; bool semaphore; };

long eventfd_create_fd(struct proc *p, unsigned initval, int flags) {
    if (flags & ~(O_NONBLOCK_ | O_CLOEXEC_ | EFD_SEMAPHORE))
        return -EINVAL;
    struct eventfd *e = kmalloc(sizeof(*e));
    if (!e)
        return -ENOMEM;
    e->counter = initval;
    e->semaphore = flags & EFD_SEMAPHORE;
    int fd = file_install_special(p, FILE_EVENTFD, e, (flags & O_NONBLOCK_) ? O_NONBLOCK_ : 0,
                                  !!(flags & O_CLOEXEC_));
    if (fd < 0)
        kfree(e);
    return fd;
}

static long eventfd_read(struct open_file *f, void *buf, size_t count) {
    struct eventfd *e = file_special(f);
    if (count < 8)
        return -EINVAL;
    while (!e->counter) {
        if (file_status_flags(f) & O_NONBLOCK_)
            return -EAGAIN;
        if (file_poll_wait_until(0) < 0)
            return -4;                      /* EINTR */
    }
    uint64_t v = e->semaphore ? 1 : e->counter;
    e->counter -= v;
    memcpy(buf, &v, 8);
    file_poll_notify();
    return 8;
}

static long eventfd_write(struct open_file *f, const void *buf, size_t count) {
    struct eventfd *e = file_special(f);
    if (count < 8)
        return -EINVAL;
    uint64_t v;
    memcpy(&v, buf, 8);
    if (v == UINT64_MAX)
        return -EINVAL;
    while (e->counter > UINT64_MAX - 1 - v) {   /* would overflow: wait for a reader */
        if (file_status_flags(f) & O_NONBLOCK_)
            return -EAGAIN;
        if (file_poll_wait_until(0) < 0)
            return -4;
    }
    e->counter += v;
    file_poll_notify();
    return 8;
}

/* ------------------------------------------------------------------ */
/* timerfd */
struct timerfd {
    int clockid;
    bool armed;
    uint64_t next_ns;        /* uptime-ns domain */
    uint64_t interval_ns;
    uint64_t expirations;
};

static uint64_t tfd_now(void) { return timer_uptime_ns(); }

/* Fold elapsed time into the expiration count. */
static void tfd_update(struct timerfd *t) {
    if (!t->armed)
        return;
    uint64_t now = tfd_now();
    if (now < t->next_ns)
        return;
    if (t->interval_ns) {
        uint64_t n = 1 + (now - t->next_ns) / t->interval_ns;
        t->expirations += n;
        t->next_ns += n * t->interval_ns;
    } else {
        t->expirations += 1;
        t->armed = false;
    }
}

long timerfd_create_fd(struct proc *p, int clockid, int flags) {
    if (clockid != CLOCK_REALTIME_ && clockid != CLOCK_MONOTONIC_)
        return -EINVAL;
    if (flags & ~(O_NONBLOCK_ | O_CLOEXEC_))
        return -EINVAL;
    struct timerfd *t = kmalloc(sizeof(*t));
    if (!t)
        return -ENOMEM;
    memset(t, 0, sizeof(*t));
    t->clockid = clockid;
    int fd = file_install_special(p, FILE_TIMERFD, t, (flags & O_NONBLOCK_) ? O_NONBLOCK_ : 0,
                                  !!(flags & O_CLOEXEC_));
    if (fd < 0)
        kfree(t);
    return fd;
}

static struct timerfd *fd_timerfd(struct proc *p, int fd) {
    struct open_file *f = file_description_rw(p, fd);
    return f && file_kind(f) == FILE_TIMERFD ? file_special(f) : NULL;
}

static uint64_t spec_ns(uint64_t sec, uint64_t nsec) { return sec * 1000000000ULL + nsec; }

long timerfd_settime_fd(struct proc *p, int fd, int flags,
                        const uint64_t new_spec[4], uint64_t old_spec[4]) {
    struct timerfd *t = fd_timerfd(p, fd);
    if (!t)
        return -EINVAL;
    if (flags & ~TFD_TIMER_ABSTIME)
        return -EINVAL;
    if (new_spec[1] >= 1000000000ULL || new_spec[3] >= 1000000000ULL)
        return -EINVAL;
    if (old_spec) {                      /* {interval_s, interval_ns, value_s, value_ns} */
        tfd_update(t);
        uint64_t rem = 0;
        if (t->armed) {
            uint64_t now = tfd_now();
            rem = t->next_ns > now ? t->next_ns - now : 0;
        }
        old_spec[0] = t->interval_ns / 1000000000ULL;
        old_spec[1] = t->interval_ns % 1000000000ULL;
        old_spec[2] = rem / 1000000000ULL;
        old_spec[3] = rem % 1000000000ULL;
    }
    uint64_t value = spec_ns(new_spec[2], new_spec[3]);
    t->interval_ns = spec_ns(new_spec[0], new_spec[1]);
    t->expirations = 0;
    if (!value) {                        /* disarm */
        t->armed = false;
    } else {
        uint64_t now = tfd_now();
        if (flags & TFD_TIMER_ABSTIME) {
            if (t->clockid == CLOCK_REALTIME_) {
                int64_t rt = timer_realtime_ns();
                int64_t delta = (int64_t)value - rt;      /* time until the target */
                t->next_ns = delta > 0 ? now + (uint64_t)delta : now;
            } else {
                t->next_ns = value;
            }
        } else {
            t->next_ns = now + value;
        }
        t->armed = true;
    }
    file_poll_notify();
    return 0;
}

long timerfd_gettime_fd(struct proc *p, int fd, uint64_t cur[4]) {
    struct timerfd *t = fd_timerfd(p, fd);
    if (!t)
        return -EINVAL;
    tfd_update(t);
    uint64_t rem = 0;
    if (t->armed) {
        uint64_t now = tfd_now();
        rem = t->next_ns > now ? t->next_ns - now : 0;
    }
    cur[0] = t->interval_ns / 1000000000ULL;
    cur[1] = t->interval_ns % 1000000000ULL;
    cur[2] = rem / 1000000000ULL;
    cur[3] = rem % 1000000000ULL;
    return 0;
}

static long timerfd_read(struct open_file *f, void *buf, size_t count) {
    struct timerfd *t = file_special(f);
    if (count < 8)
        return -EINVAL;
    for (;;) {
        tfd_update(t);
        if (t->expirations)
            break;
        if (file_status_flags(f) & O_NONBLOCK_)
            return -EAGAIN;
        uint64_t deadline = 0;
        if (t->armed) {
            uint64_t now = tfd_now();
            uint64_t ns = t->next_ns > now ? t->next_ns - now : 0;
            deadline = timer_ticks() + (ns * timer_ticks_per_second() + 999999999ULL) / 1000000000ULL + 1;
        }
        if (file_poll_wait_until(deadline) < 0)
            return -4;
    }
    uint64_t n = t->expirations;
    t->expirations = 0;
    memcpy(buf, &n, 8);
    return 8;
}

/* ------------------------------------------------------------------ */
/* signalfd */
struct signalfd { uint64_t mask; };

struct ssi {                         /* struct signalfd_siginfo, 128 bytes */
    uint32_t signo;
    int32_t  err;
    int32_t  code;
    uint32_t pid;
    uint32_t uid;
    int32_t  fd;
    uint32_t tid;
    uint32_t band;
    uint32_t overrun;
    uint32_t trapno;
    int32_t  status;
    int32_t  int_;
    uint64_t ptr;
    uint64_t utime;
    uint64_t stime;
    uint64_t addr;
    uint8_t  pad[48];
};
_Static_assert(sizeof(struct ssi) == 128, "signalfd_siginfo is 128 bytes");

long signalfd_create_fd(struct proc *p, int fd, uint64_t mask, int flags) {
    if (flags & ~(O_NONBLOCK_ | O_CLOEXEC_))
        return -EINVAL;
    mask &= ~((1ULL << (9 - 1)) | (1ULL << (19 - 1)));   /* SIGKILL, SIGSTOP can't be caught */
    if (fd != -1) {
        struct open_file *f = file_description_rw(p, fd);
        if (!f || file_kind(f) != FILE_SIGNALFD)
            return -EINVAL;
        ((struct signalfd *)file_special(f))->mask = mask;
        file_poll_notify();
        return fd;
    }
    struct signalfd *s = kmalloc(sizeof(*s));
    if (!s)
        return -ENOMEM;
    s->mask = mask;
    int nfd = file_install_special(p, FILE_SIGNALFD, s, (flags & O_NONBLOCK_) ? O_NONBLOCK_ : 0,
                                   !!(flags & O_CLOEXEC_));
    if (nfd < 0)
        kfree(s);
    return nfd;
}

static long signalfd_read(struct open_file *f, void *buf, size_t count) {
    struct signalfd *s = file_special(f);
    if (count < sizeof(struct ssi))
        return -EINVAL;
    struct thread *t = my_thread();
    size_t done = 0;
    while (done + sizeof(struct ssi) <= count) {
        struct signal_info info;
        if (!signal_take(t, s->mask, &info)) {
            if (done)
                break;
            if (file_status_flags(f) & O_NONBLOCK_)
                return -EAGAIN;
            if (file_poll_wait_until(0) < 0)
                return -4;
            continue;
        }
        struct ssi out;
        memset(&out, 0, sizeof(out));
        out.signo = (uint32_t)info.signo;
        out.err = info.error;
        out.code = info.code;
        out.pid = (uint32_t)info.sender_pid;
        out.uid = (uint32_t)info.sender_uid;
        out.status = info.child_status;
        out.addr = info.fault_address;
        memcpy((uint8_t *)buf + done, &out, sizeof(out));
        done += sizeof(out);
    }
    return (long)done;
}

/* ------------------------------------------------------------------ */
/* epoll */
struct epent {
    struct epent *next;
    struct open_file *f;               /* not retained: purged on last close */
    int fd;
    uint32_t events;
    uint64_t data;
    uint32_t last;                     /* edge-triggered: ready bits seen last scan */
    uint64_t seen_gen;                 /* poll generation at the last report */
    bool disabled;                     /* EPOLLONESHOT fired */
};

struct epoll { struct epent *head; struct epoll *next; };

static struct epoll *all_epolls;

long epoll_create_fd(struct proc *p, int flags) {
    if (flags & ~O_CLOEXEC_)
        return -EINVAL;
    struct epoll *e = kmalloc(sizeof(*e));
    if (!e)
        return -ENOMEM;
    memset(e, 0, sizeof(*e));
    int fd = file_install_special(p, FILE_EPOLL, e, 0, !!(flags & O_CLOEXEC_));
    if (fd < 0) {
        kfree(e);
        return fd;
    }
    e->next = all_epolls;
    all_epolls = e;
    return fd;
}

static struct epent *ep_find(struct epoll *e, struct open_file *f, int fd) {
    for (struct epent *x = e->head; x; x = x->next)
        if (x->f == f && x->fd == fd)
            return x;
    return NULL;
}

long epoll_ctl_fd(struct proc *p, int epfd, int op, int fd, const struct kepoll_event *ev) {
    struct open_file *ef = file_description_rw(p, epfd);
    if (!ef || file_kind(ef) != FILE_EPOLL)
        return -EBADF;
    struct open_file *tf = file_description_rw(p, fd);
    if (!tf)
        return -EBADF;
    if (tf == ef || file_kind(tf) == FILE_EPOLL)
        return -EINVAL;
    struct epoll *e = file_special(ef);
    struct epent *x = ep_find(e, tf, fd);

    if (op == EPOLL_CTL_DEL) {
        if (!x)
            return -ENOENT;
        for (struct epent **pp = &e->head; *pp; pp = &(*pp)->next)
            if (*pp == x) { *pp = x->next; break; }
        kfree(x);
        return 0;
    }
    if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_MOD)
        return -EINVAL;
    if (!ev)
        return -EFAULT;
    if (!file_pollable(tf))
        return -EPERM;                     /* regular files and directories */
    if (op == EPOLL_CTL_ADD) {
        if (x)
            return -EEXIST;
        x = kmalloc(sizeof(*x));
        if (!x)
            return -ENOMEM;
        memset(x, 0, sizeof(*x));
        x->f = tf;
        x->fd = fd;
        x->next = e->head;
        e->head = x;
    } else if (!x) {
        return -ENOENT;
    }
    x->events = ev->events;
    x->data = ev->data;
    x->last = 0;
    x->disabled = false;
    file_poll_notify();
    return 0;
}

void epoll_forget_file(struct open_file *f) {
    for (struct epoll *e = all_epolls; e; e = e->next) {
        for (struct epent **pp = &e->head; *pp;) {
            if ((*pp)->f == f) {
                struct epent *dead = *pp;
                *pp = dead->next;
                kfree(dead);
            } else {
                pp = &(*pp)->next;
            }
        }
    }
}

static long epoll_scan(struct epoll *e, struct kepoll_event *out, int max,
                       uint64_t *next_ns) {
    long n = 0;
    *next_ns = 0;
    for (struct epent *x = e->head; x && n < max; x = x->next) {
        if (x->disabled)
            continue;
        short rev = 0;
        short want = (short)(x->events & 0xFFFF);
        file_poll_description(x->f, want, &rev);
        uint32_t ready = (uint32_t)(unsigned short)rev & (x->events | POLLERR_ | POLLHUP_);
        if (x->events & EPOLLET) {
            /* A new edge is a newly ready bit, or any readiness change since we
             * last reported (data arriving on an fd that was already readable).
             * Unrelated activity can cause a spurious repeat, which edge-
             * triggered users must tolerate anyway. */
            uint64_t gen = file_poll_generation();
            uint32_t fresh = ready & ~x->last;
            x->last = ready;
            if (ready && (fresh || gen != x->seen_gen))
                x->seen_gen = gen;
            else
                ready = 0;
        }
        if (!ready) {
            uint64_t ev_ns = evfd_next_event_ns(x->f);
            if (ev_ns && (!*next_ns || ev_ns < *next_ns))
                *next_ns = ev_ns;
            continue;
        }
        out[n].events = ready;
        out[n].data = x->data;
        n++;
        if (x->events & EPOLLONESHOT)
            x->disabled = true;
    }
    return n;
}

long epoll_wait_fd(struct proc *p, int epfd, struct kepoll_event *out, int max,
                   int64_t timeout_ms) {
    struct open_file *ef = file_description_rw(p, epfd);
    if (!ef || file_kind(ef) != FILE_EPOLL)
        return -EBADF;
    if (max <= 0)
        return -EINVAL;
    if (max > EPOLL_MAX_EVENTS_PER_CALL)
        max = EPOLL_MAX_EVENTS_PER_CALL;
    struct epoll *e = file_special(ef);

    uint64_t hz = timer_ticks_per_second();
    uint64_t user_deadline = 0;                 /* in ticks; 0 = none */
    if (timeout_ms > 0) {
        uint64_t ticks = ((uint64_t)timeout_ms * hz + 999) / 1000;
        user_deadline = timer_ticks() + (ticks ? ticks : 1);
    }
    for (;;) {
        uint64_t next_ns;
        long n = epoll_scan(e, out, max, &next_ns);
        if (n > 0)
            return n;
        if (timeout_ms == 0)
            return 0;
        uint64_t now_t = timer_ticks();
        if (user_deadline && now_t >= user_deadline)
            return 0;
        uint64_t deadline = user_deadline;
        if (next_ns) {                          /* wake for a timerfd expiry */
            uint64_t now = timer_uptime_ns();
            uint64_t ns = next_ns > now ? next_ns - now : 0;
            uint64_t t = now_t + (ns * hz + 999999999ULL) / 1000000000ULL + 1;
            if (!deadline || t < deadline)
                deadline = t;
        }
        if (file_poll_wait_until(deadline) < 0)
            return -4;                          /* EINTR */
    }
}

/* ------------------------------------------------------------------ */
long evfd_read(struct open_file *f, void *buffer, size_t count) {
    switch (file_kind(f)) {
        case FILE_EVENTFD:  return eventfd_read(f, buffer, count);
        case FILE_TIMERFD:  return timerfd_read(f, buffer, count);
        case FILE_SIGNALFD: return signalfd_read(f, buffer, count);
        default:            return -EINVAL;      /* epoll descriptors cannot be read */
    }
}

long evfd_write(struct open_file *f, const void *buffer, size_t count) {
    return file_kind(f) == FILE_EVENTFD ? eventfd_write(f, buffer, count) : -EINVAL;
}

void evfd_poll(struct open_file *f, int events, short *revents) {
    short r = 0;
    switch (file_kind(f)) {
        case FILE_EVENTFD: {
            struct eventfd *e = file_special(f);
            if (e->counter) r |= POLLIN_;
            if (e->counter < UINT64_MAX - 1) r |= POLLOUT_;
            break;
        }
        case FILE_TIMERFD: {
            struct timerfd *t = file_special(f);
            tfd_update(t);
            if (t->expirations) r |= POLLIN_;
            break;
        }
        case FILE_SIGNALFD: {
            struct signalfd *s = file_special(f);
            if (signal_pending_set(my_thread()) & s->mask) r |= POLLIN_;
            break;
        }
        case FILE_EPOLL: {
            struct epoll *e = file_special(f);
            struct kepoll_event tmp[1];
            uint64_t ns;
            if (epoll_scan(e, tmp, 1, &ns) > 0) r |= POLLIN_;
            break;
        }
        default:
            break;
    }
    *revents = (short)(r & (events | POLLERR_ | POLLHUP_));
}

uint64_t evfd_next_event_ns(struct open_file *f) {
    if (file_kind(f) != FILE_TIMERFD)
        return 0;
    struct timerfd *t = file_special(f);
    tfd_update(t);
    return (t->armed && !t->expirations) ? t->next_ns : 0;
}

void evfd_release(struct open_file *f) {
    void *priv = file_special(f);
    if (file_kind(f) == FILE_EPOLL) {
        struct epoll *e = priv;
        for (struct epoll **pp = &all_epolls; *pp; pp = &(*pp)->next)
            if (*pp == e) { *pp = e->next; break; }
        while (e->head) {
            struct epent *x = e->head;
            e->head = x->next;
            kfree(x);
        }
    }
    kfree(priv);
}
