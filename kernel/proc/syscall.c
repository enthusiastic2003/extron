#include <kernel/proc/syscall.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <kernel/console.h>
#include <kernel/mm/uvm.h>
#include <kernel/mm/paging.h>
#include <kernel/drivers/timer.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/tty.h>
#include <kernel/fs/tar.h>
#include <kernel/proc/exec.h>
#include <kernel/proc/futex.h>
#include <kernel/fs/file.h>
#include <kernel/fs/vfs.h>
#include <kernel/errno.h>
#include <kernel/klibc/string.h>
#include <arch/irq_spinlock.h>
#include <stdbool.h>

/*
 * Every handler receives the trap frame, not just its three arguments.
 *
 * fork() and execve() are the reason: neither can be expressed as
 * "compute a value, return it in x0". fork() has to duplicate the
 * caller's entire saved register state into a second process, and
 * execve() has to rewrite where the caller resumes. Both of those live
 * in the frame SAVE_CONTEXT built, so both need to see it.
 */
typedef uint64_t (*syscall_fn)(uint64_t, uint64_t, uint64_t, struct aarch64_frame *);

/* boot.S sets TCR_EL1.T0SZ=16, so TTBR0 — the calling process's own
 * table — translates VA[0, 2^48). At or above this is either the
 * kernel's TTBR1 half or the non-canonical hole between them. */
#define USER_VA_LIMIT (1ULL << 48)

/*
 * Validate a user-supplied buffer before the kernel dereferences it.
 *
 * Syscalls run at EL1 with TTBR1 live, so a raw user pointer is not
 * merely untrusted, it is *powerful*: without this check a process
 * could pass a kernel address to SYS_WRITE and dump kernel memory to
 * the console, or pass one to SYS_READ and have the kernel write
 * attacker-chosen bytes into its own data structures. Neither is
 * exotic — they're a one-instruction change to any test payload.
 *
 * Two separate things are checked:
 *
 *  - the range lies entirely below USER_VA_LIMIT, and doesn't wrap.
 *    This is what keeps the kernel half unreachable.
 *  - every page is actually mapped in *this* process's table. Without
 *    it, a bad-but-user-range pointer faults at EL1, and a Data Abort
 *    there is a panic (kernel/arch/aarch64/exceptions.c) — i.e. any
 *    process could halt the machine with one bad pointer.
 *
 * Validate-then-use is race-free here only because the whole syscall
 * runs DAIF-masked: nothing can unmap these pages between the check
 * and the access, since nothing else runs at all. That is the same
 * invariant sys_write()'s atomicity rests on — see its comment. If
 * syscalls are ever made preemptible, this becomes a TOCTOU window
 * and has to move to a real copy_from_user()/copy_to_user() that
 * faults safely instead of checking up front.
 */
static bool user_buffer_ok(struct proc *p, uint64_t addr, uint64_t len) {
    if (len == 0)
        return true;
    if (!p || addr >= USER_VA_LIMIT || len > USER_VA_LIMIT - addr)
        return false;

    uint64_t start = addr & ~((uint64_t)PAGE_SIZE - 1);
    uint64_t end   = (addr + len + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
    for (uint64_t v = start; v < end; v += PAGE_SIZE) {
        if (!virt_to_phys(p->ttbr0, v))
            return false;
    }
    return true;
}

/* Runs start-to-finish with DAIF masked — the SVC handler masks on
 * entry and nothing in this path unmasks, so the byte loop below cannot
 * be preempted and no second writer can interleave with it. That makes
 * sys_write atomic per call without a lock. The property is incidental
 * rather than designed, and load-bearing: unmask during syscalls (which
 * is worth doing eventually, since a long write currently blocks every
 * interrupt for its duration — at 115200 baud the PL011's 16-byte RX
 * FIFO overruns after ~1.4ms of unserviced input) and this needs a real
 * console lock, and user_buffer_ok() above needs to become a faulting
 * copy_to_user(). Both change together or neither does. */
static uint64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t count,
                          struct aarch64_frame *f) {
    (void)f;
    if (!user_buffer_ok(my_proc(), buf_addr, count)) {
        kprintf("[SYSCALL write] rejected buffer %p (+%lu) from pid %lu\n",
                (void *)buf_addr, (unsigned long)count,
                my_proc() ? (unsigned long)my_proc()->pid : 0);
        return (uint64_t)-1;
    }
    return (uint64_t)file_write(my_proc(), (int)fd,
                                (const void *)buf_addr, count);
}

/* x86 needs this because FS_BASE (its TLS base) historically requires
 * ring-0 help to set. AArch64's equivalent, TPIDR_EL0, is architecturally
 * designed to be directly read/write from EL0 (`msr tpidr_el0, x0`) —
 * mlibc's own generic aarch64 sysdeps almost certainly just do that
 * directly rather than trapping here at all. Kept as a working syscall
 * anyway, for ABI-number symmetry with x86 and as a fallback, even
 * though it may never actually get called. */
static uint64_t sys_tcb_set(uint64_t addr, uint64_t arg2, uint64_t arg3,
                            struct aarch64_frame *f) {
    (void)arg2;
    (void)arg3;
    (void)f;
    __asm__ volatile ("msr tpidr_el0, %0" :: "r"(addr) : "memory");
    return 0;
}

/*
 * Terminate the caller. The process becomes a ZOMBIE rather than
 * disappearing: its exit status has to survive until its parent asks
 * for it, and — more practically — nothing can free a process's kernel
 * stack or page tables while it is the thing currently standing on
 * them. proc_destroy() therefore runs later, in the PARENT's context,
 * out of sys_wait().
 *
 * Setting the calling thread away from THREAD_RUNNING keeps it out of the
 * rotation permanently: schedule() only re-enqueues `old` if it is
 * still THREAD_RUNNING when called, so no separate queue removal is needed.
 */
static uint64_t sys_exit(uint64_t status, uint64_t arg2, uint64_t arg3,
                         struct aarch64_frame *f) {
    (void)arg2;
    (void)arg3;
    (void)f;
    struct proc *p = my_proc();
    kprintf("[SYSCALL exit] pid=%lu status=%lu\n",
            p ? (unsigned long)p->pid : 0, (unsigned long)status);
    proc_exit_current((int)(status & 0xff));
}

struct user_thread_create_args {
    uint64_t entry;
    uint64_t user_sp;
    uint64_t tls;
    uint64_t exit_word;
};

static uint64_t sys_gettid(uint64_t a, uint64_t b, uint64_t c,
                           struct aarch64_frame *f) {
    (void)a; (void)b; (void)c; (void)f;
    return my_thread() ? my_thread()->tid : (uint64_t)-1;
}

static uint64_t sys_thread_create(uint64_t args_addr, uint64_t b, uint64_t c,
                                  struct aarch64_frame *f) {
    (void)b; (void)c; (void)f;
    struct proc *p = my_proc();
    if (!user_buffer_ok(p, args_addr, sizeof(struct user_thread_create_args)))
        return (uint64_t)-1;
    struct user_thread_create_args args =
        *(const struct user_thread_create_args *)args_addr;
    if (!args.entry || !args.user_sp
            || !user_buffer_ok(p, args.entry, 1)
            || !user_buffer_ok(p, args.user_sp, 3 * sizeof(uint64_t))
            || (args.tls && !user_buffer_ok(p, args.tls, 1))
            || (args.exit_word
                && !user_buffer_ok(p, args.exit_word, sizeof(int))))
        return (uint64_t)-1;

    struct thread *t = proc_thread_create(p, args.entry, args.user_sp,
                                          args.tls, args.exit_word);
    return t ? t->tid : (uint64_t)-1;
}

static uint64_t sys_thread_exit(uint64_t a, uint64_t b, uint64_t c,
                                struct aarch64_frame *f) {
    (void)a; (void)b; (void)c; (void)f;
    struct proc *p = my_proc();
    struct thread *t = my_thread();
    if (!p || !t)
        return (uint64_t)-1;

    if (t->exit_word) {
        __atomic_store_n((int *)t->exit_word, 1, __ATOMIC_RELEASE);
        futex_wake(p, (int *)t->exit_word);
    }
    bool last = proc_thread_is_last_live(p, t);
    thread_set_exited(t);
    wakeup(t); /* raw SYS_THREAD_JOIN waiters */
    if (last) {
        p->exit_status = 0;
        file_table_close_all(p);
        proc_mark_exited(p);
        signal_notify_parent(p, 1, 0); /* CLD_EXITED */
        if (p->parent)
            wakeup(p->parent);
    }
    schedule();
    for (;;) __asm__ volatile ("");
}

static uint64_t sys_thread_join(uint64_t tid, uint64_t b, uint64_t c,
                                struct aarch64_frame *f) {
    (void)b; (void)c; (void)f;
    struct proc *p = my_proc();
    if (!p || tid == my_thread()->tid)
        return (uint64_t)-1;
    for (;;) {
        struct thread *target = proc_thread_lookup(p, tid);
        if (!target)
            return (uint64_t)-1;
        if (target->state == THREAD_EXITED)
            return proc_thread_reap(p, tid) == 0 ? 0 : (uint64_t)-1;
        my_thread()->chan = target;
        my_thread()->sleep_until = 0;
        thread_set_sleeping(my_thread());
        schedule();
    }
}

static uint64_t sys_futex_wait(uint64_t word_addr, uint64_t expected,
                               uint64_t timeout_ms, struct aarch64_frame *f) {
    (void)f;
    struct proc *p = my_proc();
    if ((word_addr & (sizeof(int) - 1))
            || !user_buffer_ok(p, word_addr, sizeof(int)))
        return (uint64_t)-1;
    uint64_t deadline = 0;
    if (timeout_ms) {
        uint64_t hz = timer_ticks_per_second();
        uint64_t delta = (timeout_ms * hz + 999) / 1000;
        deadline = timer_ticks() + (delta ? delta : 1);
    }
    return (uint64_t)futex_wait(p, (int *)word_addr, (int)expected, deadline);
}

static uint64_t sys_futex_wake(uint64_t word_addr, uint64_t b, uint64_t c,
                               struct aarch64_frame *f) {
    (void)b; (void)c; (void)f;
    struct proc *p = my_proc();
    if ((word_addr & (sizeof(int) - 1))
            || !user_buffer_ok(p, word_addr, sizeof(int)))
        return (uint64_t)-1;
    return (uint64_t)futex_wake(p, (int *)word_addr);
}

struct user_sigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask[16];
};

static uint64_t sys_sigaction(uint64_t signo, uint64_t action_addr,
                              uint64_t old_addr, struct aarch64_frame *f) {
    (void)f;
    struct proc *p = my_proc();
    if (signo < 1 || signo > SIGNAL_MAX)
        return (uint64_t)-1;
    if (old_addr) {
        if (!user_buffer_ok(p, old_addr, sizeof(struct user_sigaction)))
            return (uint64_t)-1;
        struct signal_action old;
        if (signal_action_get(p, (int)signo, &old) != 0)
            return (uint64_t)-1;
        struct user_sigaction *out = (struct user_sigaction *)old_addr;
        memset(out, 0, sizeof(*out));
        out->handler = old.handler;
        out->flags = old.flags;
        out->restorer = old.restorer;
        out->mask[0] = old.mask;
    }
    if (action_addr) {
        if (!user_buffer_ok(p, action_addr, sizeof(struct user_sigaction)))
            return (uint64_t)-1;
        const struct user_sigaction *in =
            (const struct user_sigaction *)action_addr;
        struct signal_action action = {
            .handler = in->handler,
            .flags = in->flags,
            .restorer = in->restorer,
            .mask = in->mask[0],
        };
        if (action.handler > SIGNAL_IGN
                && (!user_buffer_ok(p, action.handler, 1)
                    || !user_buffer_ok(p, action.restorer, 1)))
            return (uint64_t)-1;
        if (signal_action_set(p, (int)signo, &action) != 0)
            return (uint64_t)-1;
    }
    return 0;
}

static uint64_t sys_kill(uint64_t pid, uint64_t signo, uint64_t c,
                         struct aarch64_frame *f) {
    (void)c; (void)f;
    int64_t selector = (int64_t)pid;
    if (selector > 0) {
        struct proc *target = proc_lookup((uint64_t)selector);
        if (!target)
            return (uint64_t)-ESRCH;
        if (!signal_may_send(my_proc(), target, (int)signo))
            return (uint64_t)-EPERM;
        return signal_send(target, (int)signo) == 0 ? 0 : (uint64_t)-EINVAL;
    }
    uint64_t pgid = selector == 0 ? my_proc()->pgid
                                  : selector < -1 ? (uint64_t)-selector : 0;
    return pgid ? (uint64_t)signal_send_group_from(
                      my_proc(), pgid, (int)signo)
                : (uint64_t)-EINVAL;
}

static uint64_t sys_tgkill(uint64_t pid, uint64_t tid, uint64_t signo,
                           struct aarch64_frame *f) {
    (void)f;
    if ((int64_t)pid <= 0 || (int64_t)tid <= 0)
        return (uint64_t)-1;
    struct proc *target = proc_lookup(pid);
    if (!target)
        return (uint64_t)-ESRCH;
    struct thread *thread = proc_thread_lookup(target, tid);
    if (!thread)
        return (uint64_t)-ESRCH;
    if (!signal_may_send(my_proc(), target, (int)signo))
        return (uint64_t)-EPERM;
    return signal_send_thread(target, thread, (int)signo) == 0
        ? 0 : (uint64_t)-EINVAL;
}

static uint64_t sys_sigprocmask(uint64_t how, uint64_t set_addr,
                                uint64_t old_addr, struct aarch64_frame *f) {
    (void)f;
    struct proc *p = my_proc();
    uint64_t set = 0, old = 0;
    if (set_addr) {
        if (!user_buffer_ok(p, set_addr, 16 * sizeof(uint64_t)))
            return (uint64_t)-1;
        set = *(const uint64_t *)set_addr;
    }
    if (old_addr && !user_buffer_ok(p, old_addr, 16 * sizeof(uint64_t)))
        return (uint64_t)-1;
    if (signal_mask_update(my_thread(), (int)how,
                           set_addr ? &set : NULL, &old) != 0)
        return (uint64_t)-1;
    if (old_addr) {
        memset((void *)old_addr, 0, 16 * sizeof(uint64_t));
        *(uint64_t *)old_addr = old;
    }
    return 0;
}

static uint64_t sys_sigreturn(uint64_t a, uint64_t b, uint64_t c,
                              struct aarch64_frame *f) {
    (void)a; (void)b; (void)c;
    return signal_sigreturn(f);
}

/* fd 0 is the system console TTY. keyboard.c fills the raw
 * interrupt-driven ring; tty_read() owns termios policy. */
static uint64_t sys_read(uint64_t fd, uint64_t buf_addr, uint64_t count,
                         struct aarch64_frame *f) {
    (void)f;
    /* Checked before blocking, not after: kbd_getc() sleeps, and coming
     * back from that only to discover the destination was never valid
     * would mean a keystroke consumed and thrown away. */
    if (!user_buffer_ok(my_proc(), buf_addr, count)) {
        kprintf("[SYSCALL read] rejected buffer %p (+%lu) from pid %lu\n",
                (void *)buf_addr, (unsigned long)count,
                my_proc() ? (unsigned long)my_proc()->pid : 0);
        return (uint64_t)-1;
    }
    return (uint64_t)file_read(my_proc(), (int)fd, (void *)buf_addr, count);
}

#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TIOCGWINSZ 0x5413
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define POLLIN 0x0001
#define POLLOUT 0x0004
#define POLLNVAL 0x0020

struct extron_pollfd {
    int fd;
    short events;
    short revents;
};

static uint64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg,
                          struct aarch64_frame *f) {
    (void)f;
    struct proc *p = my_proc();
    if (!file_is_tty(p, (int)fd))
        return (uint64_t)-1;
    if (request == TCGETS) {
        if (!user_buffer_ok(p, arg, sizeof(struct tty_termios)))
            return (uint64_t)-1;
        tty_get_termios((struct tty_termios *)arg);
        return 0;
    }
    if (request == TCSETS || request == TCSETSW || request == TCSETSF) {
        if (!user_buffer_ok(p, arg, sizeof(struct tty_termios)))
            return (uint64_t)-1;
        tty_set_termios((const struct tty_termios *)arg, request == TCSETSF);
        return 0;
    }
    if (request == TIOCGWINSZ) {
        if (!user_buffer_ok(p, arg, sizeof(struct tty_winsize)))
            return (uint64_t)-1;
        tty_get_winsize((struct tty_winsize *)arg);
        return 0;
    }
    if (request == TIOCGPGRP) {
        if (!user_buffer_ok(p, arg, sizeof(int)))
            return (uint64_t)-1;
        *(int *)arg = (int)tty_foreground_pgid();
        return 0;
    }
    if (request == TIOCSPGRP) {
        if (!user_buffer_ok(p, arg, sizeof(int)))
            return (uint64_t)-1;
        int pgid = *(const int *)arg;
        if (pgid <= 0 || !proc_group_exists((uint64_t)pgid, p->sid))
            return (uint64_t)-1;
        tty_set_foreground_pgid((uint64_t)pgid);
        return 0;
    }
    return (uint64_t)-1;
}

static int poll_scan(struct extron_pollfd *fds, size_t count) {
    int ready = 0;
    for (size_t i = 0; i < count; i++) {
        fds[i].revents = 0;
        if (fds[i].fd >= 0
                && file_poll(my_proc(), fds[i].fd, fds[i].events,
                             &fds[i].revents) < 0)
            fds[i].revents = POLLNVAL;
        if (fds[i].revents)
            ready++;
    }
    return ready;
}

static uint64_t sys_poll(uint64_t fds_addr, uint64_t count, uint64_t timeout_raw,
                         struct aarch64_frame *f) {
    (void)f;
    if (count > 64 || count > (uint64_t)-1 / sizeof(struct extron_pollfd))
        return (uint64_t)-1;
    size_t bytes = (size_t)count * sizeof(struct extron_pollfd);
    if (!user_buffer_ok(my_proc(), fds_addr, bytes))
        return (uint64_t)-1;
    struct extron_pollfd *fds = (struct extron_pollfd *)fds_addr;
    int ready = poll_scan(fds, count);
    int timeout = (int)timeout_raw;
    if (ready || timeout == 0)
        return (uint64_t)ready;

    bool waits_for_input = false;
    for (size_t i = 0; i < count; i++)
        if (file_is_tty(my_proc(), fds[i].fd)
                && (fds[i].events & POLLIN))
            waits_for_input = true;
    if (!waits_for_input)
        return 0;
    kbd_wait_for_input(timeout);
    if (signal_pending_unblocked(my_thread()))
        return (uint64_t)-4; /* EINTR */
    return (uint64_t)poll_scan(fds, count);
}

#define O_CLOEXEC 02000000
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030

static uint64_t sys_pipe(uint64_t fds_addr, uint64_t flags, uint64_t c,
                         struct aarch64_frame *f) {
    (void)c; (void)f;
    if (!user_buffer_ok(my_proc(), fds_addr, 2 * sizeof(int)))
        return (uint64_t)-1;
    int fds[2];
    if (file_pipe(my_proc(), fds, (int)flags) != 0)
        return (uint64_t)-1;
    ((int *)fds_addr)[0] = fds[0];
    ((int *)fds_addr)[1] = fds[1];
    return 0;
}

static uint64_t sys_dup(uint64_t oldfd, uint64_t flags, uint64_t c,
                        struct aarch64_frame *f) {
    (void)c; (void)f;
    if (flags & ~O_CLOEXEC)
        return (uint64_t)-1;
    return (uint64_t)file_dup(my_proc(), (int)oldfd, 0,
                              !!(flags & O_CLOEXEC));
}

static uint64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t flags,
                         struct aarch64_frame *f) {
    (void)f;
    if (flags & ~O_CLOEXEC)
        return (uint64_t)-1;
    return (uint64_t)file_dup2(my_proc(), (int)oldfd, (int)newfd,
                               !!(flags & O_CLOEXEC));
}

static uint64_t sys_fcntl(uint64_t fd, uint64_t request, uint64_t arg,
                          struct aarch64_frame *f) {
    (void)f;
    switch (request) {
        case F_DUPFD:
            return (uint64_t)file_dup(my_proc(), (int)fd, (int)arg, 0);
        case F_DUPFD_CLOEXEC:
            return (uint64_t)file_dup(my_proc(), (int)fd, (int)arg, 1);
        case F_GETFD:
            return (uint64_t)file_get_fd_flags(my_proc(), (int)fd);
        case F_SETFD:
            return (uint64_t)file_set_fd_flags(my_proc(), (int)fd, (int)arg);
        case F_GETFL:
            return (uint64_t)file_get_status_flags(my_proc(), (int)fd);
        case F_SETFL:
            return (uint64_t)file_set_status_flags(my_proc(), (int)fd, (int)arg);
        default:
            return (uint64_t)-1;
    }
}

/* Ticks-from-time using the actually configured Hz (timer_ticks_per_
 * second()), not a hardcoded frequency baked in twice like x86's own
 * version. Runs fully DAIF-masked (this whole handler is inside the
 * SVC exception path, which masks DAIF on entry same as any other
 * exception) so setting sleep_until/chan/state here needs no lock —
 * thread_wakeup_expired() (kernel/drivers/timer.c's IRQ handler) simply
 * can't run concurrently with this. */
static uint64_t sys_sleep(uint64_t seconds, uint64_t nanos, uint64_t arg3,
                          struct aarch64_frame *f) {
    (void)arg3;
    (void)f;
    struct thread *t = my_thread();
    unsigned hz = timer_ticks_per_second();
    uint64_t ticks = seconds * hz + (nanos * hz) / 1000000000ULL;
    if (ticks == 0)
        ticks = 1;
    t->chan = NULL;
    t->sleep_until = timer_ticks() + ticks;
    thread_set_sleeping(t);
    schedule();
    return signal_pending_unblocked(t) ? (uint64_t)-4 : 0; /* EINTR */
}

static uint64_t sys_proc_dump(uint64_t a, uint64_t b, uint64_t c,
                              struct aarch64_frame *f) {
    (void)a;
    (void)b;
    (void)c;
    (void)f;
    proc_dump_table();
    return 0;
}

static uint64_t sys_anon_alloc(uint64_t size, uint64_t arg2, uint64_t arg3,
                               struct aarch64_frame *f) {
    (void)arg2;
    (void)arg3;
    (void)f;
    struct proc *p = my_proc();
    return (uint64_t)vm_allocate_region(p->mm, size, VM_READ | VM_WRITE | VM_USER);
}

static uint64_t sys_anon_free(uint64_t addr, uint64_t size, uint64_t arg3,
                              struct aarch64_frame *f) {
    (void)arg3;
    (void)f;
    struct proc *p = my_proc();
    vm_free_region(p->mm, addr, size);
    return 0;
}

/* Monotonic milliseconds since boot. Takes no pointer, so nothing to
 * validate — the result goes back in x0.
 *
 * Deliberately NOT derived from timer_ticks(): the timer runs at 20Hz,
 * so that path can only resolve 50ms, and a game clock built on it would
 * be quantised well below what it needs (DOOM's tic rate is 35/sec,
 * ~28.6ms). timer_uptime_ms() reads CNTPCT_EL0 instead — see its comment
 * in kernel/drivers/timer.c. */
static uint64_t sys_uptime_ms(uint64_t a, uint64_t b, uint64_t c,
                              struct aarch64_frame *f) {
    (void)a;
    (void)b;
    (void)c;
    (void)f;
    return timer_uptime_ms();
}

static uint64_t sys_clock_get(uint64_t clock, uint64_t b, uint64_t c,
                              struct aarch64_frame *f) {
    (void)b; (void)c; (void)f;
    if (clock == 0)
        return (uint64_t)timer_realtime_ns();
    if (clock == 1)
        return timer_uptime_ns();
    return (uint64_t)-EINVAL;
}

static uint64_t sys_clock_set(uint64_t clock, uint64_t seconds,
                              uint64_t nanos, struct aarch64_frame *f) {
    (void)f;
    if (clock != 0 || (int64_t)nanos < 0 || nanos >= 1000000000ULL)
        return (uint64_t)-EINVAL;
    struct proc *p = my_proc();
    irq_spin_lock(&p->cred_lock);
    bool privileged = p->euid == 0;
    irq_spin_unlock(&p->cred_lock);
    if (!privileged)
        return (uint64_t)-EPERM;
    int64_t secs = (int64_t)seconds;
    if (secs > INT64_MAX / 1000000000LL
            || secs < INT64_MIN / 1000000000LL)
        return (uint64_t)-EINVAL;
    timer_set_realtime_ns(secs * 1000000000LL + (int64_t)nanos);
    return 0;
}

/*
 * Map an initrd file read-only into the caller and return a pointer to
 * its first byte; *out_size receives the length.
 *
 * A view, not a copy. The initrd is already resident in RAM and already
 * reachable through the kernel's HHDM, so this costs page-table entries
 * and nothing else — which is the entire reason a multi-megabyte DOOM
 * WAD can be opened without implementing fopen/fread/fseek first.
 *
 * Mapped without VM_WRITE: the initrd is shared by every process and
 * is the kernel's own copy, so a writable mapping would let one process
 * corrupt what everything else reads. Without VM_EXEC too, so
 * arch_translate_vm_flags() marks it NX — data should never be
 * executable, and a WAD is the archetypal attacker-controlled blob.
 */
static uint64_t sys_map_initrd(uint64_t name_addr, uint64_t name_len,
                               uint64_t out_size_addr,
                               struct aarch64_frame *frame) {
    (void)frame;
    struct proc *p = my_proc();

    /* TAR_NAME_MAX-ish: tar's own name field is 100 bytes, so anything
     * longer cannot name a real entry. Bounding here also keeps the
     * copy below on a fixed-size stack buffer. */
    char name[101];
    if (name_len >= sizeof(name))
        return 0;
    if (!user_buffer_ok(p, name_addr, name_len) ||
        !user_buffer_ok(p, out_size_addr, sizeof(uint64_t)))
        return 0;

    /* Copy in before use: the name is validated as a range, but reading
     * it twice (once to check, once to use) is the shape TOCTOU bugs
     * take, and tar_open() would otherwise hold a user pointer. */
    memcpy(name, (const void *)name_addr, name_len);
    name[name_len] = '\0';

    struct tar_file f;
    if (!tar_open(name, &f)) {
        kprintf("[SYSCALL map_initrd] '%s' not found\n", name);
        return 0;
    }

    /* tar_open() hands back an HHDM pointer (kernel/fs/tar.c sets
     * tar_start = mod_start + NEW_HDDM), so the physical address is just
     * that minus the offset. */
    phys_addr_t phys = (phys_addr_t)((uint64_t)f.data - NEW_HDDM);

    virt_addr_t va = vm_map_region(p->mm, phys, f.size, VM_READ | VM_USER);
    if (!va)
        return 0;

    *(uint64_t *)out_size_addr = (uint64_t)f.size;
    return (uint64_t)va;
}

/*
 * Copy a NUL-terminated string out of user memory.
 *
 * Length isn't known up front, so the range can't be validated in one
 * shot the way user_buffer_ok() handles a counted buffer. Instead the
 * page is re-validated each time the read crosses into a new one, which
 * costs one page-table walk per 4KB rather than one per byte, and
 * refuses rather than faulting at EL1 if the string runs off the end of
 * its mapping — an unterminated string at the very top of a mapped page
 * is exactly how a process would otherwise turn a bad pointer into a
 * kernel panic.
 *
 * Returns the length excluding the terminator, or -1.
 */
static long copy_user_string(struct proc *p, uint64_t uaddr, char *dst, size_t max) {
    if (max == 0)
        return -EINVAL;
    uint64_t checked_page = ~0ULL;

    for (size_t i = 0; i < max; i++) {
        uint64_t va   = uaddr + i;
        uint64_t page = va & ~((uint64_t)PAGE_SIZE - 1);
        if (page != checked_page) {
            if (!user_buffer_ok(p, page, 1))
                return -EFAULT;
            checked_page = page;
        }
        dst[i] = *(const char *)va;
        if (dst[i] == '\0')
            return (long)i;
    }
    return -ENAMETOOLONG;   /* no terminator within `max` */
}

static uint64_t sys_open(uint64_t path_addr, uint64_t flags, uint64_t mode,
                         struct aarch64_frame *f) {
    (void)f;
    char path[VFS_PATH_MAX + 1];
    long result = copy_user_string(my_proc(), path_addr, path, sizeof(path));
    if (result < 0)
        return (uint64_t)result;
    return (uint64_t)file_open(my_proc(), path, (int)flags, (uint32_t)mode);
}

static uint64_t sys_close(uint64_t fd, uint64_t b, uint64_t c,
                          struct aarch64_frame *f) {
    (void)b; (void)c; (void)f;
    return (uint64_t)file_close(my_proc(), (int)fd);
}

static uint64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence,
                          struct aarch64_frame *f) {
    (void)f;
    return (uint64_t)file_seek(my_proc(), (int)fd, (int64_t)offset, (int)whence);
}

static uint64_t sys_mkdir(uint64_t path_addr, uint64_t mode, uint64_t c,
                          struct aarch64_frame *f) {
    (void)c; (void)f;
    char path[VFS_PATH_MAX + 1];
    long result = copy_user_string(my_proc(), path_addr, path, sizeof(path));
    if (result < 0)
        return (uint64_t)result;
    struct vfs_path cwd;
    if (proc_cwd_snapshot(my_proc(), &cwd) < 0)
        return (uint64_t)-EIO;
    struct vfs_cred cred;
    proc_vfs_cred_snapshot(my_proc(), &cred);
    result = vfs_mkdir(&cwd, path,
                       (uint32_t)mode & ~proc_get_umask(my_proc()), &cred);
    vfs_path_release(&cwd);
    return (uint64_t)result;
}

static uint64_t sys_unlink_common(uint64_t path_addr, int directory) {
    char path[VFS_PATH_MAX + 1];
    struct proc *p = my_proc();
    long result = copy_user_string(p, path_addr, path, sizeof(path));
    if (result < 0)
        return (uint64_t)result;
    struct vfs_path cwd;
    if (proc_cwd_snapshot(p, &cwd) < 0)
        return (uint64_t)-EIO;
    struct vfs_cred cred;
    proc_vfs_cred_snapshot(p, &cred);
    result = vfs_unlink(&cwd, path, directory, &cred);
    vfs_path_release(&cwd);
    return (uint64_t)result;
}

static uint64_t sys_unlink(uint64_t path_addr, uint64_t b, uint64_t c,
                           struct aarch64_frame *f) {
    (void)b; (void)c; (void)f;
    return sys_unlink_common(path_addr, 0);
}

static uint64_t sys_rmdir(uint64_t path_addr, uint64_t b, uint64_t c,
                          struct aarch64_frame *f) {
    (void)b; (void)c; (void)f;
    return sys_unlink_common(path_addr, 1);
}

static uint64_t sys_rename(uint64_t old_addr, uint64_t new_addr, uint64_t c,
                           struct aarch64_frame *f) {
    (void)c; (void)f;
    char old_path[VFS_PATH_MAX + 1], new_path[VFS_PATH_MAX + 1];
    struct proc *p = my_proc();
    long result = copy_user_string(p, old_addr, old_path, sizeof(old_path));
    if (result < 0)
        return (uint64_t)result;
    result = copy_user_string(p, new_addr, new_path, sizeof(new_path));
    if (result < 0)
        return (uint64_t)result;
    struct vfs_path cwd;
    if (proc_cwd_snapshot(p, &cwd) < 0)
        return (uint64_t)-EIO;
    struct vfs_cred cred;
    proc_vfs_cred_snapshot(p, &cred);
    result = vfs_rename(&cwd, old_path, new_path, &cred);
    vfs_path_release(&cwd);
    return (uint64_t)result;
}

static uint64_t sys_link(uint64_t old_addr, uint64_t new_addr, uint64_t flags,
                         struct aarch64_frame *f) {
    (void)f;
    char old_path[VFS_PATH_MAX + 1], new_path[VFS_PATH_MAX + 1];
    struct proc *p = my_proc();
    long result = copy_user_string(p, old_addr, old_path, sizeof(old_path));
    if (result < 0) return (uint64_t)result;
    result = copy_user_string(p, new_addr, new_path, sizeof(new_path));
    if (result < 0) return (uint64_t)result;
    struct vfs_path cwd;
    if (proc_cwd_snapshot(p, &cwd) < 0) return (uint64_t)-EIO;
    struct vfs_cred cred;
    proc_vfs_cred_snapshot(p, &cred);
    result = vfs_link(&cwd, old_path, &cwd, new_path, !!flags, &cred);
    vfs_path_release(&cwd);
    return (uint64_t)result;
}

static uint64_t sys_symlink(uint64_t target_addr, uint64_t link_addr, uint64_t c,
                            struct aarch64_frame *f) {
    (void)c; (void)f;
    char target[VFS_PATH_MAX + 1], link_path[VFS_PATH_MAX + 1];
    struct proc *p = my_proc();
    long result = copy_user_string(p, target_addr, target, sizeof(target));
    if (result < 0) return (uint64_t)result;
    result = copy_user_string(p, link_addr, link_path, sizeof(link_path));
    if (result < 0) return (uint64_t)result;
    struct vfs_path cwd;
    if (proc_cwd_snapshot(p, &cwd) < 0) return (uint64_t)-EIO;
    struct vfs_cred cred;
    proc_vfs_cred_snapshot(p, &cred);
    result = vfs_symlink(&cwd, target, link_path, &cred);
    vfs_path_release(&cwd);
    return (uint64_t)result;
}

static uint64_t sys_readlink(uint64_t path_addr, uint64_t buffer,
                             uint64_t size, struct aarch64_frame *f) {
    (void)f;
    struct proc *p = my_proc();
    if (!user_buffer_ok(p, buffer, size)) return (uint64_t)-EFAULT;
    char path[VFS_PATH_MAX + 1];
    long result = copy_user_string(p, path_addr, path, sizeof(path));
    if (result < 0) return (uint64_t)result;
    struct vfs_path cwd;
    if (proc_cwd_snapshot(p, &cwd) < 0) return (uint64_t)-EIO;
    struct vfs_cred cred;
    proc_vfs_cred_snapshot(p, &cred);
    result = vfs_readlink(&cwd, path, (void *)buffer, size, &cred);
    vfs_path_release(&cwd);
    return (uint64_t)result;
}

static uint64_t sys_getpid(uint64_t a, uint64_t b, uint64_t c,
                           struct aarch64_frame *f) {
    (void)a; (void)b; (void)c; (void)f;
    return my_proc()->pid;
}

static uint64_t sys_getuid(uint64_t a, uint64_t b, uint64_t c,
                           struct aarch64_frame *f) {
    (void)a; (void)b; (void)c; (void)f;
    struct proc *p = my_proc();
    irq_spin_lock(&p->cred_lock);
    uint32_t value = p->ruid;
    irq_spin_unlock(&p->cred_lock);
    return value;
}

static uint64_t sys_geteuid(uint64_t a, uint64_t b, uint64_t c,
                            struct aarch64_frame *f) {
    (void)a; (void)b; (void)c; (void)f;
    struct proc *p = my_proc();
    irq_spin_lock(&p->cred_lock);
    uint32_t value = p->euid;
    irq_spin_unlock(&p->cred_lock);
    return value;
}

static uint64_t sys_getgid(uint64_t a, uint64_t b, uint64_t c,
                           struct aarch64_frame *f) {
    (void)a; (void)b; (void)c; (void)f;
    struct proc *p = my_proc();
    irq_spin_lock(&p->cred_lock);
    uint32_t value = p->rgid;
    irq_spin_unlock(&p->cred_lock);
    return value;
}

static uint64_t sys_getegid(uint64_t a, uint64_t b, uint64_t c,
                            struct aarch64_frame *f) {
    (void)a; (void)b; (void)c; (void)f;
    struct proc *p = my_proc();
    irq_spin_lock(&p->cred_lock);
    uint32_t value = p->egid;
    irq_spin_unlock(&p->cred_lock);
    return value;
}

static uint64_t set_user_id(uint32_t uid, bool effective_only) {
    struct proc *p = my_proc();
    irq_spin_lock(&p->cred_lock);
    if (p->euid == 0 && !effective_only)
        p->ruid = p->euid = p->suid = uid;
    else if (p->euid == 0 || uid == p->ruid || uid == p->suid)
        p->euid = uid;
    else {
        irq_spin_unlock(&p->cred_lock);
        return (uint64_t)-EPERM;
    }
    irq_spin_unlock(&p->cred_lock);
    return 0;
}

static uint64_t set_group_id(uint32_t gid, bool effective_only) {
    struct proc *p = my_proc();
    irq_spin_lock(&p->cred_lock);
    if (p->euid == 0 && !effective_only)
        p->rgid = p->egid = p->sgid = gid;
    else if (p->euid == 0 || gid == p->rgid || gid == p->sgid)
        p->egid = gid;
    else {
        irq_spin_unlock(&p->cred_lock);
        return (uint64_t)-EPERM;
    }
    irq_spin_unlock(&p->cred_lock);
    return 0;
}

static uint64_t sys_setuid(uint64_t uid, uint64_t b, uint64_t c,
                           struct aarch64_frame *f) {
    (void)b; (void)c; (void)f;
    return set_user_id((uint32_t)uid, false);
}

static uint64_t sys_seteuid(uint64_t uid, uint64_t b, uint64_t c,
                            struct aarch64_frame *f) {
    (void)b; (void)c; (void)f;
    return set_user_id((uint32_t)uid, true);
}

static uint64_t sys_setgid(uint64_t gid, uint64_t b, uint64_t c,
                           struct aarch64_frame *f) {
    (void)b; (void)c; (void)f;
    return set_group_id((uint32_t)gid, false);
}

static uint64_t sys_setegid(uint64_t gid, uint64_t b, uint64_t c,
                            struct aarch64_frame *f) {
    (void)b; (void)c; (void)f;
    return set_group_id((uint32_t)gid, true);
}

static uint64_t sys_getgroups(uint64_t size, uint64_t list_addr, uint64_t c,
                              struct aarch64_frame *f) {
    (void)c; (void)f;
    struct proc *p = my_proc();
    irq_spin_lock(&p->cred_lock);
    size_t count = p->supplementary_group_count;
    if (!size) {
        irq_spin_unlock(&p->cred_lock);
        return count;
    }
    if (size < count || !user_buffer_ok(p, list_addr,
                                        count * sizeof(uint32_t))) {
        irq_spin_unlock(&p->cred_lock);
        return (uint64_t)(size < count ? -EINVAL : -EFAULT);
    }
    memcpy((void *)list_addr, p->supplementary_groups,
           count * sizeof(uint32_t));
    irq_spin_unlock(&p->cred_lock);
    return count;
}

static uint64_t sys_setgroups(uint64_t size, uint64_t list_addr, uint64_t c,
                              struct aarch64_frame *f) {
    (void)c; (void)f;
    struct proc *p = my_proc();
    if (size > VFS_GROUP_MAX)
        return (uint64_t)-EINVAL;
    if (size && !user_buffer_ok(p, list_addr, size * sizeof(uint32_t)))
        return (uint64_t)-EFAULT;
    irq_spin_lock(&p->cred_lock);
    if (p->euid != 0) {
        irq_spin_unlock(&p->cred_lock);
        return (uint64_t)-EPERM;
    }
    if (size)
        memcpy(p->supplementary_groups, (const void *)list_addr,
               size * sizeof(uint32_t));
    p->supplementary_group_count = size;
    irq_spin_unlock(&p->cred_lock);
    return 0;
}

static uint64_t sys_umask(uint64_t mask, uint64_t b, uint64_t c,
                          struct aarch64_frame *f) {
    (void)b; (void)c; (void)f;
    return proc_set_umask(my_proc(), (uint32_t)mask);
}

static uint64_t sys_getppid(uint64_t a, uint64_t b, uint64_t c,
                            struct aarch64_frame *f) {
    (void)a; (void)b; (void)c; (void)f;
    struct proc *p = my_proc();
    return p->parent ? p->parent->pid : 0;
}

static uint64_t sys_getpgid(uint64_t pid, uint64_t b, uint64_t c,
                            struct aarch64_frame *f) {
    (void)b; (void)c; (void)f;
    struct proc *caller = my_proc();
    struct proc *target = pid ? proc_lookup(pid) : caller;
    if (!target || target->sid != caller->sid)
        return (uint64_t)-1;
    return target->pgid;
}

static uint64_t sys_setpgid(uint64_t pid, uint64_t pgid, uint64_t c,
                            struct aarch64_frame *f) {
    (void)c; (void)f;
    struct proc *caller = my_proc();
    struct proc *target = pid ? proc_lookup(pid) : caller;
    if (!target || (target != caller && target->parent != caller)
            || target->sid != caller->sid || target->pid == target->sid)
        return (uint64_t)-1;
    if (!pgid)
        pgid = target->pid;
    if (pgid != target->pid && !proc_group_exists(pgid, caller->sid))
        return (uint64_t)-1;
    target->pgid = pgid;
    return 0;
}

static uint64_t sys_setsid(uint64_t a, uint64_t b, uint64_t c,
                           struct aarch64_frame *f) {
    (void)a; (void)b; (void)c; (void)f;
    struct proc *p = my_proc();
    if (p->pgid == p->pid)
        return (uint64_t)-1;
    p->sid = p->pid;
    p->pgid = p->pid;
    return p->sid;
}

static uint64_t sys_getcwd(uint64_t buffer, uint64_t size, uint64_t c,
                           struct aarch64_frame *f) {
    (void)c; (void)f;
    struct proc *p = my_proc();
    if (!size || !user_buffer_ok(p, buffer, size))
        return (uint64_t)-EFAULT;
    struct vfs_path cwd;
    if (proc_cwd_snapshot(p, &cwd) < 0)
        return (uint64_t)-EIO;
    int result = vfs_get_path(&cwd, (char *)buffer, (size_t)size);
    vfs_path_release(&cwd);
    return (uint64_t)(result == -ENAMETOOLONG ? -ERANGE : result);
}

static uint64_t sys_chdir(uint64_t path_addr, uint64_t b, uint64_t c,
                          struct aarch64_frame *f) {
    (void)b; (void)c; (void)f;
    char path[VFS_PATH_MAX + 1];
    struct proc *p = my_proc();
    long copied = copy_user_string(p, path_addr, path, sizeof(path));
    if (copied < 0)
        return (uint64_t)copied;
    struct vfs_path cwd;
    if (proc_cwd_snapshot(p, &cwd) < 0)
        return (uint64_t)-EIO;
    struct vfs_path resolved;
    struct vfs_cred cred;
    proc_vfs_cred_snapshot(p, &cred);
    int result = vfs_lookup_path_as(&cwd, path, 1, &cred, &resolved);
    vfs_path_release(&cwd);
    if (result < 0)
        return (uint64_t)result;
    if (resolved.dentry->node->type != VFS_NODE_DIRECTORY) {
        vfs_path_release(&resolved);
        return (uint64_t)-ENOTDIR;
    }
    result = vfs_check_access(resolved.dentry->node, &cred, VFS_ACCESS_EXEC);
    if (result < 0) {
        vfs_path_release(&resolved);
        return (uint64_t)result;
    }
    proc_cwd_set(p, &resolved);
    vfs_path_release(&resolved);
    return 0;
}

static uint64_t sys_fchdir(uint64_t fd, uint64_t b, uint64_t c,
                           struct aarch64_frame *f) {
    (void)b; (void)c; (void)f;
    struct proc *p = my_proc();
    struct vfs_path path;
    int result = file_get_path(p, (int)fd, &path);
    if (result < 0) return (uint64_t)result;
    struct vfs_cred cred;
    proc_vfs_cred_snapshot(p, &cred);
    result = vfs_check_access(path.dentry->node, &cred, VFS_ACCESS_EXEC);
    if (result == 0)
        proc_cwd_set(p, &path);
    vfs_path_release(&path);
    return (uint64_t)result;
}

static uint64_t sys_readdir(uint64_t fd, uint64_t buffer, uint64_t size,
                            struct aarch64_frame *f) {
    (void)f;
    if (!user_buffer_ok(my_proc(), buffer, size)) return (uint64_t)-EFAULT;
    return (uint64_t)file_readdir(my_proc(), (int)fd, (void *)buffer, size);
}

struct extron_stat {
    uint64_t dev, ino;
    uint32_t mode;
    uint64_t nlink;
    uint32_t uid, gid;
    uint64_t rdev, pad1;
    int64_t size, blksize;
    int32_t pad2;
    int64_t blocks;
    struct { int64_t sec, nsec; } atim, mtim, ctim;
    int32_t pad3[2];
};

struct path_at_request {
    uint64_t op;
    int64_t dirfd1;
    uint64_t path1;
    int64_t dirfd2;
    uint64_t path2;
    uint64_t buffer;
    uint64_t size;
    uint64_t flags;
};

#define AT_FDCWD_KERNEL      (-100)
#define AT_SYMLINK_NOFOLLOW_KERNEL 0x100
#define AT_REMOVEDIR_KERNEL  0x200
#define AT_SYMLINK_FOLLOW_KERNEL   0x400
#define AT_EACCESS_KERNEL          0x200
#define UTIME_NOW_KERNEL  ((1LL << 30) - 1)
#define UTIME_OMIT_KERNEL ((1LL << 30) - 2)

struct extron_timespec { int64_t sec, nsec; };

static int path_at_base(struct proc *p, int64_t dirfd, const char *path,
                        struct vfs_path *out) {
    if (path[0] == '/')
        return vfs_root_path(out);
    if (dirfd == AT_FDCWD_KERNEL)
        return proc_cwd_snapshot(p, out) < 0 ? -EIO : 0;
    return file_get_path(p, (int)dirfd, out);
}

/* Must match mlibc's AArch64 abi-bits/stat.h. In particular nlink_t is
 * 64-bit, so st_size begins at 56 rather than 48. */
_Static_assert(offsetof(struct extron_stat, size) == 56,
               "mlibc AArch64 stat: st_size offset");
_Static_assert(sizeof(struct extron_stat) == 144,
               "mlibc AArch64 stat: struct size");

static uint64_t sys_stat(uint64_t target, uint64_t value, uint64_t stat_addr,
                         struct aarch64_frame *f) {
    (void)f;
    struct proc *p = my_proc();
    if (!user_buffer_ok(p, stat_addr, sizeof(struct extron_stat)))
        return (uint64_t)-EFAULT;
    struct vfs_attr attr;
    if (target == 0 || target == 2) {
        char path[VFS_PATH_MAX + 1];
        long copied = copy_user_string(p, value, path, sizeof(path));
        if (copied < 0)
            return (uint64_t)copied;
        struct vfs_path cwd;
        if (proc_cwd_snapshot(p, &cwd) < 0)
            return (uint64_t)-EIO;
        struct vfs_cred cred;
        proc_vfs_cred_snapshot(p, &cred);
        int result = target == 2 ? vfs_lstat(&cwd, path, &attr, &cred)
                                 : vfs_stat(&cwd, path, &attr, &cred);
        vfs_path_release(&cwd);
        if (result < 0)
            return (uint64_t)result;
    } else if (target == 1) {
        if (file_info(p, (int)value, &attr) != 0)
            return (uint64_t)-EBADF;
    } else {
        return (uint64_t)-EINVAL;
    }
    struct extron_stat *st = (struct extron_stat *)stat_addr;
    memset(st, 0, sizeof(*st));
    st->ino = attr.ino;
    st->mode = (attr.type == VFS_NODE_DIRECTORY ? 0040000
              : attr.type == VFS_NODE_SYMLINK ? 0120000
              : attr.type == VFS_NODE_DEVICE ? 0020000 : 0100000)
             | attr.mode;
    st->nlink = attr.nlink;
    st->uid = attr.uid;
    st->gid = attr.gid;
    st->size = (int64_t)attr.size;
    st->blksize = 4096;
    st->blocks = (int64_t)((attr.size + 511) / 512);
    st->atim.sec = attr.atime.sec;
    st->atim.nsec = attr.atime.nsec;
    st->mtim.sec = attr.mtime.sec;
    st->mtim.nsec = attr.mtime.nsec;
    st->ctim.sec = attr.ctime.sec;
    st->ctim.nsec = attr.ctime.nsec;
    return 0;
}

static void store_extron_stat(struct extron_stat *st,
                              const struct vfs_attr *attr) {
    memset(st, 0, sizeof(*st));
    st->ino = attr->ino;
    st->mode = (attr->type == VFS_NODE_DIRECTORY ? 0040000
              : attr->type == VFS_NODE_SYMLINK ? 0120000
              : attr->type == VFS_NODE_DEVICE ? 0020000 : 0100000)
             | attr->mode;
    st->nlink = attr->nlink;
    st->uid = attr->uid;
    st->gid = attr->gid;
    st->size = (int64_t)attr->size;
    st->blksize = 4096;
    st->blocks = (int64_t)((attr->size + 511) / 512);
    st->atim.sec = attr->atime.sec;
    st->atim.nsec = attr->atime.nsec;
    st->mtim.sec = attr->mtime.sec;
    st->mtim.nsec = attr->mtime.nsec;
    st->ctim.sec = attr->ctime.sec;
    st->ctim.nsec = attr->ctime.nsec;
}

static uint64_t sys_path_at(uint64_t request_addr, uint64_t b, uint64_t c,
                            struct aarch64_frame *f) {
    (void)b; (void)c; (void)f;
    struct proc *p = my_proc();
    if (!user_buffer_ok(p, request_addr, sizeof(struct path_at_request)))
        return (uint64_t)-EFAULT;
    struct path_at_request request = *(struct path_at_request *)request_addr;
    struct vfs_cred cred;
    proc_vfs_cred_snapshot(p, &cred);
    char path1[VFS_PATH_MAX + 1], path2[VFS_PATH_MAX + 1];
    long result = copy_user_string(p, request.path1, path1, sizeof(path1));
    if (result < 0) return (uint64_t)result;
    if (request.op == 2 || request.op == 3 || request.op == 4) {
        result = copy_user_string(p, request.path2, path2, sizeof(path2));
        if (result < 0) return (uint64_t)result;
    }

    struct vfs_path base1 = {0}, base2 = {0};
    if (request.op != 4) {
        result = path_at_base(p, request.dirfd1, path1, &base1);
        if (result < 0) return (uint64_t)result;
    }
    if (request.op == 2 || request.op == 3 || request.op == 4) {
        result = path_at_base(p, request.dirfd2, path2, &base2);
        if (result < 0) {
            vfs_path_release(&base1);
            return (uint64_t)result;
        }
    }

    switch (request.op) {
        case 1:
            if (request.flags & ~AT_REMOVEDIR_KERNEL) result = -EINVAL;
            else result = vfs_unlink(&base1, path1,
                !!(request.flags & AT_REMOVEDIR_KERNEL), &cred);
            break;
        case 2:
            if (request.flags) result = -EINVAL;
            else result = vfs_rename_at(&base1, path1, &base2, path2, &cred);
            break;
        case 3:
            if (request.flags & ~AT_SYMLINK_FOLLOW_KERNEL) result = -EINVAL;
            else result = vfs_link(&base1, path1, &base2, path2,
                !!(request.flags & AT_SYMLINK_FOLLOW_KERNEL), &cred);
            break;
        case 4:
            if (request.flags) result = -EINVAL;
            else result = vfs_symlink(&base2, path1, path2, &cred);
            break;
        case 5:
            if (request.flags || !user_buffer_ok(p, request.buffer, request.size))
                result = request.flags ? -EINVAL : -EFAULT;
            else result = vfs_readlink(&base1, path1,
                (void *)request.buffer, request.size, &cred);
            break;
        case 6: {
            if (request.flags & ~AT_SYMLINK_NOFOLLOW_KERNEL)
                result = -EINVAL;
            else if (!user_buffer_ok(p, request.buffer,
                                      sizeof(struct extron_stat)))
                result = -EFAULT;
            else {
                struct vfs_attr attr;
                result = request.flags & AT_SYMLINK_NOFOLLOW_KERNEL
                    ? vfs_lstat(&base1, path1, &attr, &cred)
                    : vfs_stat(&base1, path1, &attr, &cred);
                if (result == 0)
                    store_extron_stat((struct extron_stat *)request.buffer,
                                      &attr);
            }
            break;
        }
        case 7:
            result = file_open_at(p, &base1, path1, (int)request.flags,
                                  (uint32_t)request.size);
            break;
        case 8:
            if (request.flags) result = -EINVAL;
            else result = vfs_mkdir(&base1, path1,
                (uint32_t)request.size & ~proc_get_umask(p), &cred);
            break;
        case 9: {
            if (request.flags & ~(AT_EACCESS_KERNEL
                                  | AT_SYMLINK_NOFOLLOW_KERNEL)) {
                result = -EINVAL;
                break;
            }
            struct vfs_cred access_cred;
            if (request.flags & AT_EACCESS_KERNEL)
                proc_vfs_cred_snapshot(p, &access_cred);
            else
                proc_vfs_real_cred_snapshot(p, &access_cred);
            result = vfs_access_path(&base1, path1, (int)request.size,
                !(request.flags & AT_SYMLINK_NOFOLLOW_KERNEL), &access_cred);
            break;
        }
        case 10: {
            if (request.flags & ~AT_SYMLINK_NOFOLLOW_KERNEL) {
                result = -EINVAL;
                break;
            }
            struct vfs_path found;
            result = vfs_lookup_path_as(&base1, path1,
                !(request.flags & AT_SYMLINK_NOFOLLOW_KERNEL), &cred, &found);
            if (result == 0) {
                result = vfs_chmod_node(found.dentry->node,
                                        (uint32_t)request.size, &cred);
                vfs_path_release(&found);
            }
            break;
        }
        case 11: {
            if (request.flags & ~AT_SYMLINK_NOFOLLOW_KERNEL) {
                result = -EINVAL;
                break;
            }
            struct vfs_path found;
            result = vfs_lookup_path_as(&base1, path1,
                !(request.flags & AT_SYMLINK_NOFOLLOW_KERNEL), &cred, &found);
            if (result == 0) {
                result = vfs_chown_node(found.dentry->node,
                    (uint32_t)request.buffer, (uint32_t)request.size, &cred);
                vfs_path_release(&found);
            }
            break;
        }
        case 12: {
            if (request.flags & ~AT_SYMLINK_NOFOLLOW_KERNEL) {
                result = -EINVAL;
                break;
            }
            struct vfs_path found;
            result = vfs_lookup_path_as(&base1, path1,
                !(request.flags & AT_SYMLINK_NOFOLLOW_KERNEL), &cred, &found);
            if (result < 0) break;
            struct vfs_attr current;
            result = vfs_getattr(found.dentry->node, &current);
            if (result < 0) {
                vfs_path_release(&found);
                break;
            }
            struct vfs_timestamp atime = current.atime;
            struct vfs_timestamp mtime = current.mtime;
            bool explicit_times = false;
            bool change_atime = true, change_mtime = true;
            int64_t now = timer_realtime_ns();
            if (!request.buffer) {
                atime.sec = mtime.sec = now / 1000000000LL;
                atime.nsec = mtime.nsec = now % 1000000000LL;
            } else if (!user_buffer_ok(p, request.buffer,
                                      2 * sizeof(struct extron_timespec))) {
                result = -EFAULT;
            } else {
                const struct extron_timespec *times
                    = (const struct extron_timespec *)request.buffer;
                for (int i = 0; i < 2; i++)
                    if (times[i].nsec < 0
                            || (times[i].nsec >= 1000000000LL
                                && times[i].nsec != UTIME_NOW_KERNEL
                                && times[i].nsec != UTIME_OMIT_KERNEL))
                        result = -EINVAL;
                if (result >= 0) {
                    explicit_times = !(times[0].nsec == UTIME_NOW_KERNEL
                                    && times[1].nsec == UTIME_NOW_KERNEL);
                    struct vfs_timestamp *values[2] = { &atime, &mtime };
                    bool *changes[2] = { &change_atime, &change_mtime };
                    for (int i = 0; i < 2; i++) {
                        if (times[i].nsec == UTIME_OMIT_KERNEL)
                            *changes[i] = false;
                        else if (times[i].nsec == UTIME_NOW_KERNEL) {
                            values[i]->sec = now / 1000000000LL;
                            values[i]->nsec = now % 1000000000LL;
                        } else {
                            values[i]->sec = times[i].sec;
                            values[i]->nsec = times[i].nsec;
                        }
                    }
                }
            }
            if (result >= 0 && (change_atime || change_mtime)) {
                if (!change_atime) atime = current.atime;
                if (!change_mtime) mtime = current.mtime;
                result = vfs_utimens_node(found.dentry->node, &atime, &mtime,
                                           explicit_times, &cred);
            }
            vfs_path_release(&found);
            break;
        }
        default:
            result = -EINVAL;
    }
    vfs_path_release(&base2);
    vfs_path_release(&base1);
    return (uint64_t)result;
}

static uint64_t sys_fchmod(uint64_t fd, uint64_t mode, uint64_t c,
                           struct aarch64_frame *f) {
    (void)c; (void)f;
    struct proc *p = my_proc();
    struct vfs_node *node;
    int result = file_get_node(p, (int)fd, &node);
    if (result < 0) return (uint64_t)result;
    struct vfs_cred cred;
    proc_vfs_cred_snapshot(p, &cred);
    result = vfs_chmod_node(node, (uint32_t)mode, &cred);
    vfs_node_release(node);
    return (uint64_t)result;
}

static uint64_t sys_fchown(uint64_t fd, uint64_t uid, uint64_t gid,
                           struct aarch64_frame *f) {
    (void)f;
    struct proc *p = my_proc();
    struct vfs_node *node;
    int result = file_get_node(p, (int)fd, &node);
    if (result < 0) return (uint64_t)result;
    struct vfs_cred cred;
    proc_vfs_cred_snapshot(p, &cred);
    result = vfs_chown_node(node, (uint32_t)uid, (uint32_t)gid, &cred);
    vfs_node_release(node);
    return (uint64_t)result;
}

static uint64_t sys_futimens(uint64_t fd, uint64_t times_addr, uint64_t c,
                             struct aarch64_frame *f) {
    (void)c; (void)f;
    struct proc *p = my_proc();
    struct vfs_node *node;
    int result = file_get_node(p, (int)fd, &node);
    if (result < 0) return (uint64_t)result;
    struct vfs_attr current;
    result = vfs_getattr(node, &current);
    struct vfs_timestamp atime = {0}, mtime = {0};
    if (result == 0) {
        atime = current.atime;
        mtime = current.mtime;
    }
    bool explicit_times = false;
    bool change_atime = true, change_mtime = true;
    int64_t now = timer_realtime_ns();
    if (result == 0 && !times_addr) {
        atime.sec = mtime.sec = now / 1000000000LL;
        atime.nsec = mtime.nsec = now % 1000000000LL;
    } else if (result == 0 && !user_buffer_ok(
                   p, times_addr, 2 * sizeof(struct extron_timespec))) {
        result = -EFAULT;
    } else if (result == 0) {
        const struct extron_timespec *times
            = (const struct extron_timespec *)times_addr;
        explicit_times = !(times[0].nsec == UTIME_NOW_KERNEL
                        && times[1].nsec == UTIME_NOW_KERNEL);
        struct vfs_timestamp *values[2] = { &atime, &mtime };
        struct vfs_timestamp original[2] = { current.atime, current.mtime };
        bool *changes[2] = { &change_atime, &change_mtime };
        for (int i = 0; i < 2; i++) {
            if (times[i].nsec == UTIME_OMIT_KERNEL) {
                *values[i] = original[i];
                *changes[i] = false;
            } else if (times[i].nsec == UTIME_NOW_KERNEL) {
                values[i]->sec = now / 1000000000LL;
                values[i]->nsec = now % 1000000000LL;
            } else if (times[i].nsec < 0 || times[i].nsec >= 1000000000LL) {
                result = -EINVAL;
                break;
            } else {
                values[i]->sec = times[i].sec;
                values[i]->nsec = times[i].nsec;
            }
        }
    }
    if (result == 0 && (change_atime || change_mtime)) {
        struct vfs_cred cred;
        proc_vfs_cred_snapshot(p, &cred);
        result = vfs_utimens_node(node, &atime, &mtime,
                                  explicit_times, &cred);
    }
    vfs_node_release(node);
    return (uint64_t)result;
}

/*
 * fork() — the child gets a copy of everything and a return value of 0.
 *
 * The frame is what makes this expressible at all: it holds the
 * caller's entire saved register state, so proc_fork() can hand the
 * child a duplicate of it with x0 zeroed. The parent falls through and
 * returns the child's pid the ordinary way.
 */
static uint64_t sys_fork(uint64_t a, uint64_t b, uint64_t c,
                         struct aarch64_frame *f) {
    (void)a;
    (void)b;
    (void)c;
    struct proc *child = proc_fork(my_proc(), f);
    if (!child)
        return (uint64_t)-1;
    return child->pid;
}

/*
 * execve() — replace this process's program, keeping the process.
 *
 * Everything the new image needs is copied out of the old address space
 * FIRST, because the path and argv strings live in memory that is about
 * to be freed. Then proc_exec_replace() builds the replacement in full
 * and only swaps it in once it is complete, so a failure here returns
 * -1 to a caller still running its original program — the one case
 * execve is required to survive.
 *
 * envp is accepted and ignored: there is no environment to put in it
 * yet. Taking the argument now keeps the ABI in the shape libc expects,
 * so adding a real environment later doesn't renumber anything.
 *
 * On success this does not return in any normal sense. The frame is
 * rewritten so that RESTORE_CONTEXT+eret lands on the new program's
 * entry point with a fresh stack, and the value handed back is the argc
 * that was just written into f->x[0] — exception_dispatch() assigns the
 * return value there, so returning anything else would overwrite it.
 */
static uint64_t sys_execve(uint64_t path_addr, uint64_t argv_addr,
                           uint64_t envp_addr, struct aarch64_frame *f) {
    (void)envp_addr;
    struct proc *p = my_proc();

    char path[101];   /* tar's name field is 100 bytes; longer can't exist */
    if (copy_user_string(p, path_addr, path, sizeof(path)) < 0) {
        kprintf("[SYSCALL execve] unreadable or oversized path\n");
        return (uint64_t)-1;
    }

    /* argv, flattened into one kernel buffer. Both the pointer array and
     * the bytes it points at are bounded, and the bound is small enough
     * that the whole block fits in the top page of the new stack — see
     * build_arg_stack() in kernel/proc/exec.c. */
    char        argbuf[EXEC_ARG_BYTES];
    const char *args[EXEC_MAX_ARGS];
    int         argc = 0;
    size_t      used = 0;

    if (argv_addr) {
        for (;;) {
            if (argc >= EXEC_MAX_ARGS) {
                kprintf("[SYSCALL execve] too many arguments (max %d)\n",
                        EXEC_MAX_ARGS);
                return (uint64_t)-1;
            }
            uint64_t slot = argv_addr + (uint64_t)argc * sizeof(uint64_t);
            if (!user_buffer_ok(p, slot, sizeof(uint64_t)))
                return (uint64_t)-1;
            uint64_t str = *(const uint64_t *)slot;
            if (!str)
                break;                       /* argv[argc] == NULL */

            long len = copy_user_string(p, str, argbuf + used,
                                        EXEC_ARG_BYTES - used);
            if (len < 0) {
                kprintf("[SYSCALL execve] argv[%d] unreadable or too long\n", argc);
                return (uint64_t)-1;
            }
            args[argc++] = argbuf + used;
            used += (size_t)len + 1;
        }
    }

    /* An argv with no argv[0] is legal to pass and useless to receive.
     * Substitute the path, which is what the process would report as its
     * own name anyway. */
    if (argc == 0) {
        args[0] = path;
        argc = 1;
    }

    struct exec_image img;
    if (proc_exec_replace(p, path, args, argc, &img) != 0)
        return (uint64_t)-1;

    /* POSIX exec keeps only its calling thread. No sibling can run while
     * this DAIF-masked syscall replaces the address space, so removing and
     * freeing them here closes the last path back into the old image. */
    proc_terminate_other_threads(p, my_thread(), true);
    signal_process_exec(p);

    /* Start the new program from a clean register state rather than
     * inheriting the old one's. Anything left behind would be a value
     * from a program that no longer exists. */
    memset(f->x, 0, sizeof(f->x));
    f->x[0]     = img.argc;
    f->x[1]     = img.argv;
    f->elr_el1  = img.entry;
    f->spsr_el1 = 0;                  /* EL0t, all masks clear */
    f->sp_el0   = img.user_sp;

    return f->x[0];
}

/*
 * wait() — block until a child exits, then reap it.
 *
 * This is where a ZOMBIE actually goes away. proc_destroy() frees the
 * child's kernel stack and page tables, which is only safe from a
 * context that is standing on neither of them, so it has to be the
 * parent doing it and not the child itself.
 *
 * Returns the child's pid, writing its exit status through `status_addr`
 * if that is non-NULL, or -1 if the caller has no children at all —
 * which is the terminating condition for a "reap everything" loop.
 *
 * A process whose parent exits first is never reaped: nothing
 * re-parents orphans to an init process, because there is no init
 * process. That leaks the orphan's memory until reboot, and is worth
 * fixing when there's a shell to own the problem.
 */
static uint64_t sys_wait(uint64_t status_addr, uint64_t selector_raw,
                         uint64_t options_raw,
                         struct aarch64_frame *f) {
    (void)f;
    struct proc *p = my_proc();
    int64_t selector = (int64_t)selector_raw;
    int options = (int)options_raw;
    if ((options & ~(1 | 2 | 8)) != 0)
        return (uint64_t)-1;

    if (status_addr && !user_buffer_ok(p, status_addr, sizeof(int))) {
        kprintf("[SYSCALL wait] rejected status pointer %p from pid %lu\n",
                (void *)status_addr, (unsigned long)p->pid);
        return (uint64_t)-1;
    }

    for (;;) {
        bool any_children = false;
        int event_status = 0;
        struct proc *child = proc_find_waitable_child(
            p, selector, options, &event_status, &any_children);

        if (child) {
            uint64_t pid = child->pid;
            bool exited = child->exited;
            if (exited)
                proc_destroy(child);
            if (status_addr)
                *(int *)status_addr = event_status;
            return pid;
        }
        if (!any_children)
            return (uint64_t)-1;
        if (options & 1) /* WNOHANG */
            return 0;

        /* Sleep on our own address as the channel; sys_exit() wakes it.
         * No lock is taken around this for the same reason sys_sleep()
         * takes none: the whole syscall path is DAIF-masked, so a child
         * cannot exit in the window between the scan above and the state
         * change here — nothing else runs at all until schedule().
         *
         * chan must be non-NULL, or thread_wakeup_expired() (the timer's
         * handler) would treat this as a timed sleep whose deadline of 0
         * has long passed and wake it every tick. */
        struct thread *t = my_thread();
        t->chan        = p;
        t->sleep_until = 0;
        thread_set_sleeping(t);
        schedule();
    }
}

static const syscall_fn syscall_table[] = {
    [SYS_READ]       = sys_read,
    [SYS_WRITE]      = sys_write,
    [SYS_SLEEP]      = sys_sleep,
    [SYS_PROC_DUMP]  = sys_proc_dump,
    [SYS_ANON_ALLOC] = sys_anon_alloc,
    [SYS_ANON_FREE]  = sys_anon_free,
    [SYS_TCB_SET]    = sys_tcb_set,
    [SYS_EXIT]       = sys_exit,
    [SYS_FORK]       = sys_fork,
    [SYS_EXECVE]     = sys_execve,
    [SYS_UPTIME_MS]  = sys_uptime_ms,
    [SYS_MAP_INITRD] = sys_map_initrd,
    [SYS_WAIT]       = sys_wait,
    [SYS_OPEN]       = sys_open,
    [SYS_CLOSE]      = sys_close,
    [SYS_LSEEK]      = sys_lseek,
    [SYS_MKDIR]      = sys_mkdir,
    [SYS_GETPID]     = sys_getpid,
    [SYS_GETPPID]    = sys_getppid,
    [SYS_GETPGID]    = sys_getpgid,
    [SYS_SETPGID]    = sys_setpgid,
    [SYS_SETSID]     = sys_setsid,
    [SYS_GETCWD]     = sys_getcwd,
    [SYS_CHDIR]      = sys_chdir,
    [SYS_READDIR]    = sys_readdir,
    [SYS_STAT]       = sys_stat,
    [SYS_IOCTL]      = sys_ioctl,
    [SYS_POLL]       = sys_poll,
    [SYS_PIPE]       = sys_pipe,
    [SYS_DUP]        = sys_dup,
    [SYS_DUP2]       = sys_dup2,
    [SYS_FCNTL]      = sys_fcntl,
    [SYS_GETTID]     = sys_gettid,
    [SYS_THREAD_CREATE] = sys_thread_create,
    [SYS_THREAD_EXIT] = sys_thread_exit,
    [SYS_THREAD_JOIN] = sys_thread_join,
    [SYS_FUTEX_WAIT] = sys_futex_wait,
    [SYS_FUTEX_WAKE] = sys_futex_wake,
    [SYS_SIGACTION] = sys_sigaction,
    [SYS_KILL] = sys_kill,
    [SYS_SIGPROCMASK] = sys_sigprocmask,
    [SYS_SIGRETURN] = sys_sigreturn,
    [SYS_TGKILL] = sys_tgkill,
    [SYS_UNLINK] = sys_unlink,
    [SYS_RMDIR] = sys_rmdir,
    [SYS_RENAME] = sys_rename,
    [SYS_LINK] = sys_link,
    [SYS_SYMLINK] = sys_symlink,
    [SYS_READLINK] = sys_readlink,
    [SYS_PATH_AT] = sys_path_at,
    [SYS_GETUID] = sys_getuid,
    [SYS_GETEUID] = sys_geteuid,
    [SYS_GETGID] = sys_getgid,
    [SYS_GETEGID] = sys_getegid,
    [SYS_SETUID] = sys_setuid,
    [SYS_SETEUID] = sys_seteuid,
    [SYS_SETGID] = sys_setgid,
    [SYS_SETEGID] = sys_setegid,
    [SYS_GETGROUPS] = sys_getgroups,
    [SYS_SETGROUPS] = sys_setgroups,
    [SYS_UMASK] = sys_umask,
    [SYS_CLOCK_GET] = sys_clock_get,
    [SYS_CLOCK_SET] = sys_clock_set,
    [SYS_FCHMOD] = sys_fchmod,
    [SYS_FCHOWN] = sys_fchown,
    [SYS_FUTIMENS] = sys_futimens,
    [SYS_FCHDIR] = sys_fchdir,
};

#define SYSCALL_COUNT (sizeof(syscall_table) / sizeof(syscall_table[0]))

uint64_t syscall_dispatch(struct aarch64_frame *f) {
    uint64_t nr   = f->x[8];
    uint64_t arg1 = f->x[0];
    uint64_t arg2 = f->x[1];
    uint64_t arg3 = f->x[2];

    if (nr >= SYSCALL_COUNT || !syscall_table[nr]) {
        kprintf("[SYSCALL] invalid syscall number %lu\n", (unsigned long)nr);
        return (uint64_t)-1;
    }

    return syscall_table[nr](arg1, arg2, arg3, f);
}
