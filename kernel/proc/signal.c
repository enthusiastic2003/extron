#include <kernel/proc/signal.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <kernel/errno.h>
#include <kernel/mm/paging.h>
#include <kernel/klibc/string.h>
#include <arch/exceptions.h>
#include <arch/irq_spinlock.h>
#include <stddef.h>

#define USER_VA_LIMIT (1ULL << 48)
#define SIGNAL_FRAME_MAGIC 0x455854524f4e5347ULL
#define SIGKILL 9
#define SIGSTOP 19
#define SIGCONT 18
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGCHLD 17
#define SIGSEGV 11
#define SI_USER 0
#define SI_TKILL -6
#define SI_KERNEL 128
#define SEGV_MAPERR 1
#define SIGNAL_SA_NOCLDSTOP 1UL
#define FPSIMD_MAGIC 0x46508001U

struct user_sigset { uint64_t words[16]; };
struct user_stack {
    uint64_t sp;
    int32_t flags;
    uint32_t padding;
    uint64_t size;
};

/* Layout-compatible with mlibc's AArch64 siginfo_t. */
struct user_siginfo {
    int32_t signo;
    int32_t error;
    int32_t code;
    int32_t alignment;
    union {
        uint8_t padding[112];
        struct { int32_t pid, uid; } sender;
        struct { int32_t pid, uid, status; } child;
        struct { uint64_t address; int16_t address_lsb; } fault;
    } fields;
};

struct user_aarch64_ctx { uint32_t magic, size; };
struct user_fpsimd_context {
    struct user_aarch64_ctx head;
    uint32_t fpsr;
    uint32_t fpcr;
    uint64_t vregs[64];
};
struct user_mcontext {
    uint64_t fault_address;
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint8_t reserved[4096];
};
struct user_ucontext {
    uint64_t flags;
    uint64_t link;
    struct user_stack stack;
    struct user_sigset sigmask;
    struct user_mcontext mcontext;
};
struct user_signal_frame {
    uint64_t magic;
    uint64_t saved_tpidr_el0;
    struct user_siginfo info;
    struct user_ucontext ucontext;
};

_Static_assert(sizeof(struct user_siginfo) == 128, "siginfo_t ABI size");
_Static_assert(offsetof(struct user_siginfo, fields) == 16,
               "siginfo_t union ABI offset");
_Static_assert(sizeof(struct user_fpsimd_context) == 528,
               "fpsimd_context ABI size");
_Static_assert(offsetof(struct user_ucontext, mcontext) == 168,
               "ucontext_t mcontext ABI offset");
_Static_assert((sizeof(struct user_signal_frame) & 15) == 0,
               "signal frame preserves AAPCS64 stack alignment");

static bool user_range_ok(struct proc *p, uint64_t addr, uint64_t size) {
    if (!p || !size || addr >= USER_VA_LIMIT || size > USER_VA_LIMIT - addr)
        return false;
    uint64_t start = addr & ~((uint64_t)PAGE_SIZE - 1);
    uint64_t end = (addr + size + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
    for (uint64_t va = start; va < end; va += PAGE_SIZE)
        if (!virt_to_phys(p->ttbr0, va))
            return false;
    return true;
}

static uint64_t signal_bit(int signo) { return 1ULL << (signo - 1); }

static bool default_ignored(int signo) {
    return signo == 17 || signo == 18 || signo == 23 || signo == 28;
}

static bool default_stops(int signo) {
    return signo == SIGSTOP || signo == SIGTSTP
        || signo == SIGTTIN || signo == SIGTTOU;
}

static uint64_t stop_signal_bits(void) {
    return signal_bit(SIGSTOP) | signal_bit(SIGTSTP)
        | signal_bit(SIGTTIN) | signal_bit(SIGTTOU);
}

static void discard_pending(struct proc *p, uint64_t bits) {
    irq_spin_lock(&p->signal_lock);
    p->signal_pending &= ~bits;
    for (struct thread *t = p->threads; t; t = t->next_in_process)
        t->signal_pending &= ~bits;
    irq_spin_unlock(&p->signal_lock);
}

static bool default_stop_is_ready(struct proc *p, struct thread *directed,
                                  int signo) {
    bool ready = false;
    irq_spin_lock(&p->signal_lock);
    if (p->signal_actions[signo].handler == SIGNAL_DFL) {
        if (directed) {
            ready = !(directed->signal_mask & signal_bit(signo));
        } else {
            for (struct thread *t = p->threads; t; t = t->next_in_process)
                if (t->state != THREAD_EXITED
                        && !(t->signal_mask & signal_bit(signo))) {
                    ready = true;
                    break;
                }
        }
    }
    irq_spin_unlock(&p->signal_lock);
    return ready;
}

static struct signal_info make_info(int signo, int code) {
    struct proc *sender = my_proc();
    uint32_t sender_uid = 0;
    if (sender) {
        irq_spin_lock(&sender->cred_lock);
        sender_uid = sender->euid;
        irq_spin_unlock(&sender->cred_lock);
    }
    return (struct signal_info) {
        .signo = signo,
        .code = code,
        .sender_pid = sender ? sender->pid : 0,
        .sender_uid = sender_uid,
    };
}

void signal_process_init(struct proc *p) {
    p->signal_lock = (spinlock_t)SPINLOCK_INIT;
    p->signal_pending = 0;
    memset(p->signal_info, 0, sizeof(p->signal_info));
    memset(p->signal_actions, 0, sizeof(p->signal_actions));
}

void signal_process_fork(struct proc *child, const struct proc *parent) {
    child->signal_pending = 0;
    memset(child->signal_info, 0, sizeof(child->signal_info));
    memcpy(child->signal_actions, parent->signal_actions,
           sizeof(child->signal_actions));
}

void signal_process_exec(struct proc *p) {
    irq_spin_lock(&p->signal_lock);
    p->signal_pending = 0;
    memset(p->signal_info, 0, sizeof(p->signal_info));
    for (struct thread *t = p->threads; t; t = t->next_in_process) {
        t->signal_pending = 0;
        memset(t->signal_info, 0, sizeof(t->signal_info));
    }
    for (int signo = 1; signo <= SIGNAL_MAX; signo++)
        if (p->signal_actions[signo].handler != SIGNAL_IGN)
            p->signal_actions[signo] = (struct signal_action){0};
    irq_spin_unlock(&p->signal_lock);
}

int signal_action_get(struct proc *p, int signo, struct signal_action *out) {
    if (!p || !out || signo < 1 || signo > SIGNAL_MAX)
        return -1;
    irq_spin_lock(&p->signal_lock);
    *out = p->signal_actions[signo];
    irq_spin_unlock(&p->signal_lock);
    return 0;
}

int signal_action_set(struct proc *p, int signo,
                      const struct signal_action *action) {
    if (!p || !action || signo < 1 || signo > SIGNAL_MAX
            || signo == SIGKILL || signo == SIGSTOP)
        return -1;
    if (action->handler > SIGNAL_IGN && !action->restorer)
        return -1;
    irq_spin_lock(&p->signal_lock);
    p->signal_actions[signo] = *action;
    irq_spin_unlock(&p->signal_lock);
    return 0;
}

static void wake_recipient(struct thread *t, int signo) {
    if (t && t->state == THREAD_SLEEPING
            && !(t->signal_mask & signal_bit(signo))) {
        t->chan = NULL;
        t->sleep_until = 0;
        sched_policy_add(t);
    }
}

static int queue_process_signal(struct proc *target, int signo,
                                struct signal_info info) {
    irq_spin_lock(&target->signal_lock);
    struct signal_action action = target->signal_actions[signo];
    if (action.handler == SIGNAL_IGN
            || (action.handler == SIGNAL_DFL && default_ignored(signo))) {
        irq_spin_unlock(&target->signal_lock);
        return 0;
    }
    target->signal_pending |= signal_bit(signo);
    target->signal_info[signo] = info;
    struct thread *wake = NULL;
    for (struct thread *t = target->threads; t; t = t->next_in_process)
        if (t->state != THREAD_EXITED && !(t->signal_mask & signal_bit(signo))) {
            wake = t;
            break;
        }
    irq_spin_unlock(&target->signal_lock);
    wake_recipient(wake, signo);
    return 0;
}

int signal_send(struct proc *target, int signo) {
    if (!target || signo < 0 || signo > SIGNAL_MAX)
        return -1;
    if (!signo)
        return 0;
    if (signo == SIGCONT)
        discard_pending(target, stop_signal_bits());
    else if (default_stops(signo))
        discard_pending(target, signal_bit(SIGCONT));
    /* Continuing is an immediate side effect of generating SIGCONT,
     * independent of whether its disposition is default, ignored or caught.
     * SIGKILL must likewise make a stopped target runnable so it can die. */
    if (signo == SIGCONT || (signo == SIGKILL && target->stopped))
        proc_continue(target);
    if (default_stops(signo)
            && default_stop_is_ready(target, NULL, signo)) {
        proc_stop(target, signo);
        return 0;
    }
    return queue_process_signal(target, signo, make_info(signo, SI_USER));
}

void signal_notify_parent(struct proc *child, int code, int status) {
    if (!child || !child->parent)
        return;
    struct proc *parent = child->parent;
    if ((code == 5 || code == 6)
            && (parent->signal_actions[SIGCHLD].flags & SIGNAL_SA_NOCLDSTOP))
        return;
    struct signal_info info = {
        .signo = SIGCHLD,
        .code = code,
        .sender_pid = child->pid,
        .child_status = status,
    };
    queue_process_signal(parent, SIGCHLD, info);
}

int signal_send_thread(struct proc *target, struct thread *thread, int signo) {
    if (!target || !thread || thread->process != target
            || thread->state == THREAD_EXITED
            || signo < 0 || signo > SIGNAL_MAX)
        return -1;
    if (!signo)
        return 0;
    if (signo == SIGCONT)
        discard_pending(target, stop_signal_bits());
    else if (default_stops(signo))
        discard_pending(target, signal_bit(SIGCONT));
    if (signo == SIGCONT || (signo == SIGKILL && target->stopped))
        proc_continue(target);
    if (default_stops(signo)
            && default_stop_is_ready(target, thread, signo)) {
        proc_stop(target, signo);
        return 0;
    }
    struct signal_info info = make_info(signo, SI_TKILL);
    irq_spin_lock(&target->signal_lock);
    struct signal_action action = target->signal_actions[signo];
    if (action.handler == SIGNAL_IGN
            || (action.handler == SIGNAL_DFL && default_ignored(signo))) {
        irq_spin_unlock(&target->signal_lock);
        return 0;
    }
    thread->signal_pending |= signal_bit(signo);
    thread->signal_info[signo] = info;
    irq_spin_unlock(&target->signal_lock);
    wake_recipient(thread, signo);
    return 0;
}

bool signal_may_send(struct proc *sender, struct proc *target, int signo) {
    if (!sender || !target) return false;
    if (sender == target) return true;
    irq_spin_lock(&sender->cred_lock);
    uint32_t sender_ruid = sender->ruid, sender_euid = sender->euid;
    irq_spin_unlock(&sender->cred_lock);
    irq_spin_lock(&target->cred_lock);
    uint32_t target_ruid = target->ruid, target_suid = target->suid;
    irq_spin_unlock(&target->cred_lock);
    return sender_euid == 0
        || sender_ruid == target_ruid || sender_ruid == target_suid
        || sender_euid == target_ruid || sender_euid == target_suid
        || (signo == SIGCONT && sender->sid == target->sid);
}

struct group_send_context {
    struct proc *sender;
    uint64_t pgid;
    int signo;
    int matched;
    int count;
};

static void send_group_member(struct proc *p, void *opaque) {
    struct group_send_context *context = opaque;
    if (p->exited || p->pgid != context->pgid)
        return;
    context->matched++;
    if ((!context->sender
            || signal_may_send(context->sender, p, context->signo))
            && signal_send(p, context->signo) == 0)
        context->count++;
}

int signal_send_group(uint64_t pgid, int signo) {
    if (!pgid)
        return -1;
    struct group_send_context context = {
        .sender = NULL, .pgid = pgid, .signo = signo
    };
    proc_for_each(send_group_member, &context);
    if (context.count) return 0;
    return context.matched ? -EPERM : -ESRCH;
}

int signal_send_group_from(struct proc *sender, uint64_t pgid, int signo) {
    if (!sender || !pgid)
        return -1;
    struct group_send_context context = {
        .sender = sender, .pgid = pgid, .signo = signo
    };
    proc_for_each(send_group_member, &context);
    return context.count ? 0 : -1;
}

int signal_mask_update(struct thread *t, int how, const uint64_t *set,
                       uint64_t *old) {
    if (!t)
        return -1;
    if (old) *old = t->signal_mask;
    if (!set) return 0;
    uint64_t value = *set;
    value &= ~signal_bit(SIGKILL);
    value &= ~signal_bit(SIGSTOP);
    switch (how) {
        case 0: t->signal_mask |= value; break;
        case 1: t->signal_mask &= ~value; break;
        case 2: t->signal_mask = value; break;
        default: return -1;
    }
    return 0;
}

bool signal_pending_unblocked(struct thread *t) {
    if (!t || !t->process)
        return false;
    struct proc *p = t->process;
    irq_spin_lock(&p->signal_lock);
    bool pending = ((p->signal_pending | t->signal_pending)
                    & ~t->signal_mask) != 0;
    irq_spin_unlock(&p->signal_lock);
    return pending;
}

static void fill_siginfo(struct user_siginfo *out,
                         const struct signal_info *info) {
    memset(out, 0, sizeof(*out));
    out->signo = info->signo;
    out->error = info->error;
    out->code = info->code;
    if (info->signo == SIGCHLD) {
        out->fields.child.pid = (int32_t)info->sender_pid;
        out->fields.child.uid = (int32_t)info->sender_uid;
        out->fields.child.status = info->child_status;
    } else if (info->code == SI_USER || info->code == SI_TKILL) {
        out->fields.sender.pid = (int32_t)info->sender_pid;
        out->fields.sender.uid = (int32_t)info->sender_uid;
    } else {
        out->fields.fault.address = info->fault_address;
    }
}

static void fill_ucontext(struct user_signal_frame *frame,
                          const struct aarch64_frame *saved,
                          uint64_t old_mask) {
    struct user_ucontext *uc = &frame->ucontext;
    memset(uc, 0, sizeof(*uc));
    uc->sigmask.words[0] = old_mask;
    uc->mcontext.fault_address = saved->far_el1;
    memcpy(uc->mcontext.regs, saved->x, sizeof(saved->x));
    uc->mcontext.sp = saved->sp_el0;
    uc->mcontext.pc = saved->elr_el1;
    uc->mcontext.pstate = saved->spsr_el1;
    struct cpu_context state = {0};
    cpu_context_save_fpsimd(&state);
    frame->saved_tpidr_el0 = state.tpidr_el0;
    struct user_fpsimd_context *fp =
        (struct user_fpsimd_context *)uc->mcontext.reserved;
    fp->head.magic = FPSIMD_MAGIC;
    fp->head.size = sizeof(*fp);
    fp->fpsr = (uint32_t)state.fpsr;
    fp->fpcr = (uint32_t)state.fpcr;
    memcpy(fp->vregs, state.v, sizeof(fp->vregs));
}

static bool deliver(struct aarch64_frame *f, int signo,
                    struct signal_action action,
                    const struct signal_info *info, bool synchronous) {
    struct proc *p = my_proc();
    struct thread *t = my_thread();
    if (action.handler == SIGNAL_IGN) {
        if (!synchronous) return true;
        proc_exit_current(-signo);
    }
    if (action.handler == SIGNAL_DFL) {
        if (!synchronous && default_ignored(signo)) return true;
        if (!synchronous && default_stops(signo)) {
            proc_stop_current(signo);
            return true;
        }
        proc_exit_current(-signo);
    }
    uint64_t sp = (f->sp_el0 - sizeof(struct user_signal_frame)) & ~0xFULL;
    if (!user_range_ok(p, sp, sizeof(struct user_signal_frame)))
        proc_exit_current(-SIGSEGV);
    struct user_signal_frame *frame = (struct user_signal_frame *)sp;
    memset(frame, 0, sizeof(*frame));
    frame->magic = SIGNAL_FRAME_MAGIC;
    fill_siginfo(&frame->info, info);
    fill_ucontext(frame, f, t->signal_mask);
    t->signal_mask |= action.mask;
    if (!(action.flags & SIGNAL_SA_NODEFER)) t->signal_mask |= signal_bit(signo);
    if (action.flags & SIGNAL_SA_RESETHAND)
        signal_action_set(p, signo, &(struct signal_action){0});
    f->x[0] = (uint64_t)signo;
    f->x[1] = (uint64_t)&frame->info;
    f->x[2] = (uint64_t)&frame->ucontext;
    f->x[30] = action.restorer;
    f->elr_el1 = action.handler;
    f->spsr_el1 = 0;
    f->sp_el0 = sp;
    return true;
}

bool signal_deliver_pending(struct aarch64_frame *f) {
    struct proc *p = my_proc();
    struct thread *t = my_thread();
    if (!p || !t || (f->spsr_el1 & 0xf) != 0) return false;
    irq_spin_lock(&p->signal_lock);
    uint64_t thread_available = t->signal_pending & ~t->signal_mask;
    uint64_t process_available = p->signal_pending & ~t->signal_mask;
    uint64_t available = thread_available ? thread_available : process_available;
    if (!available) {
        irq_spin_unlock(&p->signal_lock);
        return false;
    }
    int signo = __builtin_ctzll(available) + 1;
    struct signal_info info;
    if (thread_available) {
        t->signal_pending &= ~signal_bit(signo);
        info = t->signal_info[signo];
    } else {
        p->signal_pending &= ~signal_bit(signo);
        info = p->signal_info[signo];
    }
    struct signal_action action = p->signal_actions[signo];
    irq_spin_unlock(&p->signal_lock);
    return deliver(f, signo, action, &info, false);
}

bool signal_deliver_sync(struct aarch64_frame *f, int signo) {
    struct proc *p = my_proc();
    struct thread *t = my_thread();
    if (!p || !t || (t->signal_mask & signal_bit(signo))) return false;
    struct signal_action action;
    if (signal_action_get(p, signo, &action) != 0
            || action.handler <= SIGNAL_IGN) return false;
    struct signal_info info = {
        .signo = signo,
        .code = signo == SIGSEGV ? SEGV_MAPERR : SI_KERNEL,
        .fault_address = f->far_el1,
    };
    return deliver(f, signo, action, &info, true);
}

uint64_t signal_sigreturn(struct aarch64_frame *f) {
    struct proc *p = my_proc();
    uint64_t sp = f->sp_el0;
    if (!user_range_ok(p, sp, sizeof(struct user_signal_frame)))
        proc_exit_current(-SIGSEGV);
    struct user_signal_frame *frame = (struct user_signal_frame *)sp;
    if (frame->magic != SIGNAL_FRAME_MAGIC)
        proc_exit_current(-SIGSEGV);
    struct user_mcontext *mc = &frame->ucontext.mcontext;
    if (mc->pc >= USER_VA_LIMIT || mc->sp >= USER_VA_LIMIT)
        proc_exit_current(-SIGSEGV);
    struct user_fpsimd_context *fp =
        (struct user_fpsimd_context *)mc->reserved;
    if (fp->head.magic != FPSIMD_MAGIC || fp->head.size != sizeof(*fp))
        proc_exit_current(-SIGSEGV);
    memcpy(f->x, mc->regs, sizeof(f->x));
    f->elr_el1 = mc->pc;
    f->sp_el0 = mc->sp;
    f->spsr_el1 = 0;
    my_thread()->signal_mask = frame->ucontext.sigmask.words[0]
        & ~signal_bit(SIGKILL) & ~signal_bit(SIGSTOP);
    struct cpu_context state = {0};
    state.tpidr_el0 = frame->saved_tpidr_el0;
    state.fpsr = fp->fpsr;
    state.fpcr = fp->fpcr;
    memcpy(state.v, fp->vregs, sizeof(state.v));
    cpu_context_restore_fpsimd(&state);
    return f->x[0];
}
