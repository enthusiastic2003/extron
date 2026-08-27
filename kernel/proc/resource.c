#include <kernel/proc/resource.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/exec.h>
#include <kernel/fs/file.h>
#include <kernel/drivers/timer.h>
#include <kernel/errno.h>
#include <kernel/klibc/string.h>
#include <arch/exceptions.h>
#include <arch/irq_spinlock.h>

static void usage_from_ns(uint64_t user_ns, uint64_t system_ns,
                          struct user_rusage *out) {
    memset(out, 0, sizeof(*out));
    out->utime.sec = (int64_t)(user_ns / 1000000000ULL);
    out->utime.usec = (int64_t)((user_ns % 1000000000ULL) / 1000ULL);
    out->stime.sec = (int64_t)(system_ns / 1000000000ULL);
    out->stime.usec = (int64_t)((system_ns % 1000000000ULL) / 1000ULL);
}

void resource_process_init(struct proc *p) {
    p->resource_lock = (spinlock_t)SPINLOCK_INIT;
    for (int i = 0; i < PROC_RLIMIT_COUNT; i++) {
        p->resource_limits[i].current = PROC_RLIM_INFINITY;
        p->resource_limits[i].maximum = PROC_RLIM_INFINITY;
    }
    p->resource_limits[PROC_RLIMIT_NOFILE].current = PROC_MAX_FDS;
    p->resource_limits[PROC_RLIMIT_NOFILE].maximum = PROC_MAX_FDS;
    p->resource_limits[PROC_RLIMIT_STACK].current = EXEC_USER_STACK_BYTES;
    p->resource_limits[PROC_RLIMIT_STACK].maximum = EXEC_USER_STACK_BYTES;
    p->resource_limits[PROC_RLIMIT_CORE].current = 0;
    p->resource_limits[PROC_RLIMIT_CORE].maximum = 0;
    p->cpu_user_ns = 0;
    p->cpu_system_ns = 0;
    p->children_user_ns = 0;
    p->children_system_ns = 0;
}

void resource_process_fork(struct proc *child, struct proc *parent) {
    irq_spin_lock(&parent->resource_lock);
    memcpy(child->resource_limits, parent->resource_limits,
           sizeof(child->resource_limits));
    irq_spin_unlock(&parent->resource_lock);
    child->cpu_user_ns = 0;
    child->cpu_system_ns = 0;
    child->children_user_ns = 0;
    child->children_system_ns = 0;
}

int resource_get_limit(struct proc *p, int resource, struct proc_rlimit *out) {
    if (!p || !out)
        return -EFAULT;
    if (resource < 0 || resource >= PROC_RLIMIT_COUNT)
        return -EINVAL;
    irq_spin_lock(&p->resource_lock);
    *out = p->resource_limits[resource];
    irq_spin_unlock(&p->resource_lock);
    return 0;
}

int resource_set_limit(struct proc *p, int resource,
                       const struct proc_rlimit *value) {
    if (!p || !value)
        return -EFAULT;
    if (resource < 0 || resource >= PROC_RLIMIT_COUNT)
        return -EINVAL;
    if (value->current > value->maximum)
        return -EINVAL;

    irq_spin_lock(&p->resource_lock);
    struct proc_rlimit old = p->resource_limits[resource];
    if (old.current == value->current && old.maximum == value->maximum) {
        irq_spin_unlock(&p->resource_lock);
        return 0;
    }
    if (resource != PROC_RLIMIT_NOFILE) {
        irq_spin_unlock(&p->resource_lock);
        return -EOPNOTSUPP;
    }
    if (value->maximum > PROC_MAX_FDS) {
        irq_spin_unlock(&p->resource_lock);
        return -EPERM;
    }

    irq_spin_lock(&p->cred_lock);
    uint32_t euid = p->euid;
    irq_spin_unlock(&p->cred_lock);
    if (value->maximum > old.maximum && euid != 0) {
        irq_spin_unlock(&p->resource_lock);
        return -EPERM;
    }
    p->resource_limits[resource] = *value;
    irq_spin_unlock(&p->resource_lock);
    return 0;
}

uint64_t resource_nofile_limit(struct proc *p) {
    if (!p)
        return 0;
    irq_spin_lock(&p->resource_lock);
    uint64_t limit = p->resource_limits[PROC_RLIMIT_NOFILE].current;
    irq_spin_unlock(&p->resource_lock);
    return limit < PROC_MAX_FDS ? limit : PROC_MAX_FDS;
}

static void account_until(struct thread *t, uint64_t now) {
    if (!t || !t->process || !t->cpu_account_start_ns)
        return;
    uint64_t delta = now >= t->cpu_account_start_ns
        ? now - t->cpu_account_start_ns : 0;
    struct proc *p = t->process;
    irq_spin_lock(&p->resource_lock);
    if (t->cpu_account_mode == RESOURCE_CPU_USER)
        p->cpu_user_ns += delta;
    else if (t->cpu_account_mode == RESOURCE_CPU_SYSTEM)
        p->cpu_system_ns += delta;
    irq_spin_unlock(&p->resource_lock);
    t->cpu_account_start_ns = now;
}

void resource_account_exception_enter(const struct aarch64_frame *frame) {
    struct thread *t = my_thread();
    if (!t || !frame || t->state != THREAD_RUNNING
            || !t->cpu_account_start_ns)
        return;
    uint64_t now = timer_uptime_ns();
    account_until(t, now);
    if ((frame->spsr_el1 & 0xf) == 0)
        t->cpu_account_mode = RESOURCE_CPU_SYSTEM;
    t->cpu_account_start_ns = now;
}

void resource_account_exception_leave(const struct aarch64_frame *frame) {
    struct thread *t = my_thread();
    if (!t || !frame || t->state != THREAD_RUNNING
            || !t->cpu_account_start_ns)
        return;
    uint64_t now = timer_uptime_ns();
    account_until(t, now);
    t->cpu_account_mode = ((frame->spsr_el1 & 0xf) == 0)
        ? RESOURCE_CPU_USER : RESOURCE_CPU_SYSTEM;
    t->cpu_account_start_ns = now;
}

void resource_account_switch(struct thread *old, struct thread *next) {
    uint64_t now = timer_uptime_ns();
    if (old) {
        account_until(old, now);
        old->cpu_account_start_ns = 0;
    }
    if (next) {
        if (next->cpu_account_mode == RESOURCE_CPU_NONE)
            next->cpu_account_mode = RESOURCE_CPU_USER;
        next->cpu_account_start_ns = now;
    }
}

void resource_get_self_usage(struct proc *p, struct user_rusage *out) {
    uint64_t user_ns = 0, system_ns = 0;
    if (!p || !out)
        return;
    irq_spin_lock(&p->resource_lock);
    user_ns = p->cpu_user_ns;
    system_ns = p->cpu_system_ns;
    struct thread *running = my_thread();
    if (running && running->process == p && running->cpu_account_start_ns) {
        uint64_t now = timer_uptime_ns();
        uint64_t delta = now >= running->cpu_account_start_ns
            ? now - running->cpu_account_start_ns : 0;
        if (running->cpu_account_mode == RESOURCE_CPU_USER)
            user_ns += delta;
        else if (running->cpu_account_mode == RESOURCE_CPU_SYSTEM)
            system_ns += delta;
    }
    irq_spin_unlock(&p->resource_lock);
    usage_from_ns(user_ns, system_ns, out);
}

void resource_get_children_usage(struct proc *p, struct user_rusage *out) {
    if (!p || !out)
        return;
    irq_spin_lock(&p->resource_lock);
    uint64_t user_ns = p->children_user_ns;
    uint64_t system_ns = p->children_system_ns;
    irq_spin_unlock(&p->resource_lock);
    usage_from_ns(user_ns, system_ns, out);
}

void resource_reap_child(struct proc *parent, struct proc *child,
                         struct user_rusage *child_usage) {
    if (!parent || !child)
        return;
    struct user_rusage own;
    resource_get_self_usage(child, &own);
    if (child_usage)
        *child_usage = own;

    irq_spin_lock(&child->resource_lock);
    uint64_t user_ns = child->cpu_user_ns + child->children_user_ns;
    uint64_t system_ns = child->cpu_system_ns + child->children_system_ns;
    irq_spin_unlock(&child->resource_lock);
    irq_spin_lock(&parent->resource_lock);
    parent->children_user_ns += user_ns;
    parent->children_system_ns += system_ns;
    irq_spin_unlock(&parent->resource_lock);
}
