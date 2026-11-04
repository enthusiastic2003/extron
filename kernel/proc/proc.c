#include <kernel/proc/proc.h>
#include <kernel/drivers/fbown.h>
#include <kernel/errno.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/futex.h>
#include <kernel/proc/signal.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/kheap.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/pmm.h>
#include <kernel/panic.h>
#include <kernel/console.h>
#include <kernel/mm/uvm.h>
#include <kernel/fs/file.h>
#include <kernel/drivers/timer.h>
#include <kernel/klibc/string.h>
#include <arch/irq_spinlock.h>
#include <stddef.h>
#include <stdbool.h>

static int thread_init(struct thread *t, struct proc *p, uint64_t tid,
                       virt_addr_t entry, virt_addr_t user_sp, uint64_t tls) {
    *t = (struct thread){0};
    t->tid = tid;
    t->state = THREAD_UNUSED;
    t->process = p;
    t->entry = entry;
    t->user_sp = user_sp;

    /* vmm_alloc_pages() rather than kmalloc(), specifically so this can
     * have a guard page. kmalloc hands back byte-granularity memory from
     * inside a shared heap block — there is no page boundary to unmap
     * and no way to make an overflow fault, so a deep call chain would
     * quietly chew through whatever liballoc placed below it.
     *
     * One extra page is allocated and immediately unmapped. Its bitmap
     * bit stays set, so nothing else claims that VA, and the stack now
     * has an unmapped page directly beneath it: overflow takes a Data
     * Abort naming the address instead of corrupting a neighbour. */
    virt_addr_t region = vmm_alloc_pages(THREAD_KERNEL_STACK_PAGES + 1);
    if (!region)
        return -1;

    phys_addr_t guard_phys = kvirt_to_phys(region);
    kunmap(region);
    if (guard_phys)
        pmm_free_page((void *)guard_phys);

    t->kernel_stack_base = region + PAGE_SIZE;
    t->kernel_stack_top  = region + (THREAD_KERNEL_STACK_PAGES + 1) * PAGE_SIZE;


    /* forkret-style bootstrap: pre-populate the saved context as if this
     * proc had already been switched out once, with lr pointing at the
     * trampoline instead of a real return address, and sp at a fresh,
     * empty stack. schedule()'s first switch to this proc is then the
     * exact same context_switch() call as every other switch — see
     * proc_bootstrap_trampoline()'s comment in sched.c. */
    arch_context_init(&t->context, t->kernel_stack_top,
                      proc_bootstrap_trampoline, tls);
    return 0;
}

static void thread_destroy(struct proc *p, struct thread *t) {
    if (!t)
        return;
    virt_addr_t guard = t->kernel_stack_base - PAGE_SIZE;
    vmm_free_pages(t->kernel_stack_base, THREAD_KERNEL_STACK_PAGES);
    vmm_free_unmapped_page(guard);
    if (t != &p->main_thread)
        kfree(t);
}

void proc_init(struct proc *p, uint64_t pid, virt_addr_t entry,
                virt_addr_t user_sp, phys_addr_t ttbr0) {
    p->pid   = pid;
    p->ttbr0 = ttbr0;
    p->mm    = NULL;
    p->user_argc = 0;
    p->user_argv = 0;
    p->parent = NULL;
    p->pgid = pid;
    p->sid = pid;
    p->comm[0] = p->exe[0] = p->cmdline[0] = '\0';
    p->cmdline_len = 0;
    p->start_ns = timer_uptime_ns();
    p->cred_lock = (spinlock_t)SPINLOCK_INIT;
    p->ruid = p->euid = p->suid = 0;
    p->rgid = p->egid = p->sgid = 0;
    p->supplementary_group_count = 0;
    p->file_umask = 022;
    resource_process_init(p);
    p->exit_status = 0;
    p->exited = false;
    p->stopped = false;
    p->stop_signal = 0;
    p->stop_event_pending = false;
    p->continue_event_pending = false;
    signal_process_init(p);
    p->cwd_lock = (spinlock_t)SPINLOCK_INIT;
    p->cwd = (struct vfs_path){0};
    if (vfs_root_path(&p->cwd) < 0)
        panic("proc_init: VFS root is not mounted");
    file_table_init(p);

    struct thread *t = &p->main_thread;
    if (thread_init(t, p, pid, entry, user_sp, 0) != 0)
        panic("proc_init: kernel stack allocation failed");
    p->threads = t;
    p->thread_count = 1;
}

void proc_vfs_cred_snapshot(struct proc *p, struct vfs_cred *out) {
    irq_spin_lock(&p->cred_lock);
    out->uid = p->euid;
    out->gid = p->egid;
    out->group_count = p->supplementary_group_count;
    memcpy(out->groups, p->supplementary_groups,
           out->group_count * sizeof(out->groups[0]));
    irq_spin_unlock(&p->cred_lock);
}

void proc_vfs_real_cred_snapshot(struct proc *p, struct vfs_cred *out) {
    irq_spin_lock(&p->cred_lock);
    out->uid = p->ruid;
    out->gid = p->rgid;
    out->group_count = p->supplementary_group_count;
    memcpy(out->groups, p->supplementary_groups,
           out->group_count * sizeof(out->groups[0]));
    irq_spin_unlock(&p->cred_lock);
}

uint32_t proc_get_umask(struct proc *p) {
    irq_spin_lock(&p->cred_lock);
    uint32_t mask = p->file_umask;
    irq_spin_unlock(&p->cred_lock);
    return mask;
}

uint32_t proc_set_umask(struct proc *p, uint32_t mask) {
    irq_spin_lock(&p->cred_lock);
    uint32_t old = p->file_umask;
    p->file_umask = mask & 0777;
    irq_spin_unlock(&p->cred_lock);
    return old;
}

int proc_cwd_snapshot(struct proc *p, struct vfs_path *out) {
    if (!p || !out)
        return -1;
    irq_spin_lock(&p->cwd_lock);
    *out = p->cwd;
    vfs_path_retain(out);
    irq_spin_unlock(&p->cwd_lock);
    return 0;
}

void proc_cwd_set(struct proc *p, const struct vfs_path *path) {
    if (!p || !path || !path->dentry)
        return;
    struct vfs_path replacement = *path;
    vfs_path_retain(&replacement);
    irq_spin_lock(&p->cwd_lock);
    struct vfs_path old = p->cwd;
    p->cwd = replacement;
    irq_spin_unlock(&p->cwd_lock);
    vfs_path_release(&old);
}

/*
 * The inverse of proc_init() plus the address space on top: kernel
 * stack, page tables, every page the process owned, its slot in the
 * table, and the struct itself.
 *
 * Never safe to call on the running process. It frees the kernel stack
 * the caller would be standing on, and hands the page tables TTBR0_EL1
 * points at back to the PMM to be reissued to somebody else while the
 * TLB still refers to them. sys_wait() runs this in the PARENT's
 * context, which is the whole reason a process becomes a ZOMBIE rather
 * than cleaning up after itself in sys_exit().
 */
void proc_destroy(struct proc *p) {
    if (!p)
        return;

    file_table_close_all(p);
    vfs_path_release(&p->cwd);
    if (p->mm) {
        vm_space_destroy(p->mm);   /* pages, then the tables under them */
        p->mm = NULL;
        p->ttbr0 = 0;
    }

    for (struct thread *t = p->threads; t;) {
        struct thread *next = t->next_in_process;
        thread_destroy(p, t);
        t = next;
    }

    proc_table_remove(p);
    kfree(p);
}

/* -------------------------------------------------------------
 * Process table — ported from x86's kernel/proc/proc.c
 * (~/extron-x86-backup/): a fixed slot array + a lock + a monotonic
 * PID counter. Slots aren't tied to PIDs; lookup is a linear scan —
 * fine for a hobby kernel's PROC_MAX_PROCS, switch to a hash if it ever
 * shows up in profiles (x86's own comment, still true here).
 * ------------------------------------------------------------- */
static struct proc *proc_table[PROC_MAX_PROCS];
static size_t       proc_table_count = 0;
static spinlock_t   proc_table_lock  = SPINLOCK_INIT;
/* PIDs start at 1, not 0. fork() reports 0 to the child and the child's
 * pid to the parent, so a process that legitimately owned pid 0 would
 * make those two answers indistinguishable. */
static uint64_t     next_id          = 1;

void proc_table_init(void) {
    irq_spin_lock(&proc_table_lock);
    for (size_t i = 0; i < PROC_MAX_PROCS; i++)
        proc_table[i] = NULL;
    proc_table_count = 0;
    next_id          = 1;
    irq_spin_unlock(&proc_table_lock);
    kprintf("[PROC] Process table initialized (capacity %u)\n", (unsigned)PROC_MAX_PROCS);
}

static bool proc_table_add_locked(struct proc *p) {
    for (size_t i = 0; i < PROC_MAX_PROCS; i++) {
        if (!proc_table[i]) {
            proc_table[i] = p;
            proc_table_count++;
            return true;
        }
    }
    return false;
}

static void proc_table_remove_locked(struct proc *p) {
    for (size_t i = 0; i < PROC_MAX_PROCS; i++) {
        if (proc_table[i] == p) {
            proc_table[i] = NULL;
            proc_table_count--;
            return;
        }
    }
}

uint64_t proc_alloc_pid(void) {
    irq_spin_lock(&proc_table_lock);
    uint64_t pid = next_id++;
    irq_spin_unlock(&proc_table_lock);
    return pid;
}

uint64_t proc_last_pid(void) {
    return next_id ? next_id - 1 : 0;
}

void proc_set_exec_info(struct proc *p, const char *path, const char *const *args, int argc) {
    size_t n = 0;
    for (int i = 0; i < argc && args; i++) {
        size_t l = strlen(args[i]) + 1;
        if (n + l > sizeof(p->cmdline))
            l = sizeof(p->cmdline) - n;                 /* truncate the tail */
        if (!l)
            break;
        memcpy(p->cmdline + n, args[i], l);
        p->cmdline[n + l - 1] = '\0';
        n += l;
    }
    p->cmdline_len = (uint16_t)n;
    size_t pl = strlen(path);
    if (pl >= sizeof(p->exe)) pl = sizeof(p->exe) - 1;
    memcpy(p->exe, path, pl);
    p->exe[pl] = '\0';
    const char *base = path;
    for (const char *c = path; *c; c++)
        if (*c == '/' && c[1]) base = c + 1;
    size_t bl = 0;
    while (base[bl] && base[bl] != '/' && bl < sizeof(p->comm) - 1) { p->comm[bl] = base[bl]; bl++; }
    p->comm[bl] = '\0';
}

size_t proc_list_pids(uint64_t *out, size_t max) {
    size_t n = 0;
    irq_spin_lock(&proc_table_lock);
    for (size_t i = 0; i < PROC_MAX_PROCS && n < max; i++)
        if (proc_table[i])
            out[n++] = proc_table[i]->pid;
    irq_spin_unlock(&proc_table_lock);
    return n;
}

void proc_count_states(size_t *total, size_t *running, size_t *blocked) {
    size_t t = 0, r = 0;
    irq_spin_lock(&proc_table_lock);
    for (size_t i = 0; i < PROC_MAX_PROCS; i++) {
        struct proc *p = proc_table[i];
        if (!p) continue;
        t++;
        if (p->exited) continue;
        bool run = false;
        for (struct thread *th = p->threads ? p->threads : &p->main_thread; th; th = th->next_in_process)
            if (th->state == THREAD_RUNNING || th->state == THREAD_RUNNABLE) run = true;
        if (run) r++;
    }
    irq_spin_unlock(&proc_table_lock);
    if (total) *total = t;
    if (running) *running = r;
    if (blocked) *blocked = 0;
}

uint64_t proc_alloc_tid(void) {
    return proc_alloc_pid();
}

struct thread *proc_thread_create(struct proc *p, virt_addr_t entry,
                                  virt_addr_t user_sp, uint64_t tls,
                                  virt_addr_t exit_word) {
    if (!p || p->exited)
        return NULL;
    struct thread *t = kmalloc(sizeof(*t));
    if (!t)
        return NULL;
    if (thread_init(t, p, proc_alloc_tid(), entry, user_sp, tls) != 0) {
        kfree(t);
        return NULL;
    }
    t->exit_word = exit_word;
    t->signal_mask = my_thread() ? my_thread()->signal_mask : 0;

    irq_spin_lock(&proc_table_lock);
    t->next_in_process = p->threads;
    p->threads = t;
    p->thread_count++;
    irq_spin_unlock(&proc_table_lock);
    sched_policy_add(t);
    return t;
}

struct thread *proc_thread_lookup(struct proc *p, uint64_t tid) {
    if (!p)
        return NULL;
    irq_spin_lock(&proc_table_lock);
    struct thread *found = NULL;
    for (struct thread *t = p->threads; t; t = t->next_in_process) {
        if (t->tid == tid) {
            found = t;
            break;
        }
    }
    irq_spin_unlock(&proc_table_lock);
    return found;
}

int proc_thread_reap(struct proc *p, uint64_t tid) {
    if (!p)
        return -1;
    irq_spin_lock(&proc_table_lock);
    struct thread **link = &p->threads;
    while (*link && (*link)->tid != tid)
        link = &(*link)->next_in_process;
    struct thread *t = *link;
    if (!t || t == my_thread() || t == &p->main_thread
            || t->state != THREAD_EXITED) {
        irq_spin_unlock(&proc_table_lock);
        return -1;
    }
    *link = t->next_in_process;
    p->thread_count--;
    irq_spin_unlock(&proc_table_lock);
    sched_policy_remove(t);
    thread_destroy(p, t);
    return 0;
}

bool proc_thread_is_last_live(struct proc *p, struct thread *self) {
    if (!p)
        return true;
    irq_spin_lock(&proc_table_lock);
    bool last = true;
    for (struct thread *t = p->threads; t; t = t->next_in_process) {
        if (t != self && t->state != THREAD_EXITED) {
            last = false;
            break;
        }
    }
    irq_spin_unlock(&proc_table_lock);
    return last;
}

void proc_terminate_other_threads(struct proc *p, struct thread *self,
                                  bool reap) {
    if (!p)
        return;

    /* Called from a DAIF-masked syscall on this single-CPU kernel, so the
     * list is stable here. Cancel futex membership before taking the process
     * table lock; futex wait takes those locks in the opposite order. */
    for (struct thread *t = p->threads; t; t = t->next_in_process) {
        if (t == self)
            continue;
        sched_policy_remove(t);
        futex_cancel_thread(t);
    }

    irq_spin_lock(&proc_table_lock);
    struct thread **link = &p->threads;
    while (*link) {
        struct thread *t = *link;
        if (t == self) {
            link = &t->next_in_process;
            continue;
        }
        t->state = THREAD_EXITED;
        t->chan = NULL;
        t->sleep_until = 0;
        if (!reap || t == &p->main_thread) {
            link = &t->next_in_process;
            continue;
        }
        *link = t->next_in_process;
        p->thread_count--;
        thread_destroy(p, t);
    }
    irq_spin_unlock(&proc_table_lock);
}

void proc_table_add(struct proc *p) {
    if (!p)
        panic("proc_table_add: null process");
    irq_spin_lock(&proc_table_lock);
    if (!proc_table_add_locked(p)) {
        irq_spin_unlock(&proc_table_lock);
        panic("proc_table_add: process table full");
    }
    irq_spin_unlock(&proc_table_lock);
}

void proc_table_remove(struct proc *p) {
    irq_spin_lock(&proc_table_lock);
    proc_table_remove_locked(p);
    irq_spin_unlock(&proc_table_lock);
}

struct proc *proc_lookup(uint64_t pid) {
    irq_spin_lock(&proc_table_lock);
    struct proc *found = NULL;
    for (size_t i = 0; i < PROC_MAX_PROCS; i++) {
        if (proc_table[i] && proc_table[i]->pid == pid) {
            found = proc_table[i];
            break;
        }
    }
    irq_spin_unlock(&proc_table_lock);
    return found;
}

/*
 * Find a ZOMBIE child of `parent` for sys_wait() to reap, and report
 * whether `parent` has any children at all.
 *
 * The second answer is what distinguishes "wait a bit longer" from
 * "there is nothing left to wait for" — without it wait() would block
 * forever the moment a caller reaped one child too many.
 *
 * Returns with the lock RELEASED: the caller immediately calls
 * proc_destroy(), which takes it again via proc_table_remove(). Safe to
 * let go of it in between only because the whole syscall path runs
 * DAIF-masked, so nothing can touch the table in that window.
 */
struct proc *proc_find_zombie_child(struct proc *parent, bool *out_any_children) {
    struct proc *found = NULL;
    bool any = false;

    irq_spin_lock(&proc_table_lock);
    for (size_t i = 0; i < PROC_MAX_PROCS; i++) {
        struct proc *p = proc_table[i];
        if (!p || p->parent != parent)
            continue;
        any = true;
        if (p->exited) {
            found = p;
            break;
        }
    }
    irq_spin_unlock(&proc_table_lock);

    if (out_any_children)
        *out_any_children = any;
    return found;
}

/* selector follows waitpid(): >0 exact PID, -1 any child, 0 caller's
 * process group, <-1 the absolute value names a process group. */
struct proc *proc_find_waitable_child(struct proc *parent, int64_t selector,
                                      int options, int *event_status,
                                      bool *out_any_children) {
    struct proc *found = NULL;
    bool any = false;
    irq_spin_lock(&proc_table_lock);
    for (size_t i = 0; i < PROC_MAX_PROCS; i++) {
        struct proc *p = proc_table[i];
        if (!p || p->parent != parent)
            continue;
        bool selected = selector == -1
            || (selector > 0 && p->pid == (uint64_t)selector)
            || (selector == 0 && p->pgid == parent->pgid)
            || (selector < -1 && p->pgid == (uint64_t)-selector);
        if (!selected)
            continue;
        any = true;
        if (p->exited) {
            *event_status = p->exit_status;
            found = p;
            break;
        }
        if ((options & 2) && p->stop_event_pending) { /* WUNTRACED */
            p->stop_event_pending = false;
            *event_status = 0x10000 | (p->stop_signal & 0xff);
            found = p;
            break;
        }
        if ((options & 8) && p->continue_event_pending) { /* WCONTINUED */
            p->continue_event_pending = false;
            *event_status = 0x20000;
            found = p;
            break;
        }
    }
    irq_spin_unlock(&proc_table_lock);
    if (out_any_children)
        *out_any_children = any;
    return found;
}

bool proc_group_exists(uint64_t pgid, uint64_t sid) {
    bool found = false;
    irq_spin_lock(&proc_table_lock);
    for (size_t i = 0; i < PROC_MAX_PROCS; i++)
        /* A group exists while any member does, zombies included (POSIX/
         * Linux): ash starts `echo x | cmd`, the leader exits at once, and
         * the second member must still be able to tcsetpgrp() to it. */
        if (proc_table[i]
                && proc_table[i]->pgid == pgid
                && proc_table[i]->sid == sid) {
            found = true;
            break;
        }
    irq_spin_unlock(&proc_table_lock);
    return found;
}

void proc_for_each(void (*fn)(struct proc *, void *), void *arg) {
    /* Callbacks may wake processes (for example, group-directed SIGCONT),
     * and wakeup() needs proc_table_lock itself.  Snapshot the table so no
     * callback runs beneath the global process-table lock.  Process objects
     * are not freed concurrently on the current single-core kernel. */
    struct proc *snapshot[PROC_MAX_PROCS];
    size_t count = 0;
    irq_spin_lock(&proc_table_lock);
    for (size_t i = 0; i < PROC_MAX_PROCS; i++) {
        if (proc_table[i])
            snapshot[count++] = proc_table[i];
    }
    irq_spin_unlock(&proc_table_lock);
    for (size_t i = 0; i < count; i++)
        fn(snapshot[i], arg);
}

static const char *thread_state_str(enum thread_state s) {
    switch (s) {
        case THREAD_UNUSED:   return "UNUSED";
        case THREAD_RUNNABLE: return "RUNNABLE";
        case THREAD_RUNNING:  return "RUNNING";
        case THREAD_SLEEPING: return "SLEEPING";
        case THREAD_STOPPED:  return "STOPPED";
        case THREAD_EXITED:   return "EXITED";
        default:              return "UNKNOWN";
    }
}

void proc_dump_table(void) {
    irq_spin_lock(&proc_table_lock);

    kprintf("\n========================================\n");
    kprintf("PID TID STATE TTBR0 CHAN\n");
    kprintf("----------------------------------------\n");
    for (size_t i = 0; i < PROC_MAX_PROCS; i++) {
        struct proc *p = proc_table[i];
        if (!p) continue;
        for (struct thread *t = p->threads; t; t = t->next_in_process)
            kprintf("%lu %lu %s 0x%lx 0x%lx\n",
                    (unsigned long)p->pid, (unsigned long)t->tid,
                    thread_state_str(t->state), (unsigned long)p->ttbr0,
                    (unsigned long)(uint64_t)t->chan);
    }
    kprintf("----------------------------------------\n");
    kprintf("TOTAL: %lu  PMM FREE: %lu pages\n========================================\n",
            (unsigned long)proc_table_count,
            (unsigned long)pmm_free_pages());

    irq_spin_unlock(&proc_table_lock);
}

/* -------------------------------------------------------------
 * State helpers
 * ------------------------------------------------------------- */
void thread_set_runnable(struct thread *t) { if (t) t->state = THREAD_RUNNABLE; }
void thread_set_running(struct thread *t)  { if (t) t->state = THREAD_RUNNING; }
void thread_set_sleeping(struct thread *t) { if (t) t->state = THREAD_SLEEPING; }
void thread_set_exited(struct thread *t)   { if (t) t->state = THREAD_EXITED; }
void proc_mark_exited(struct proc *p)      { if (p) p->exited = true; }

void proc_stop(struct proc *p, int signo) {
    if (!p || p->exited || p->stopped)
        return;
    struct thread *self = my_thread();
    bool stops_self = self && self->process == p;
    p->stopped = true;
    p->stop_signal = signo;
    p->stop_event_pending = true;
    p->continue_event_pending = false;
    for (struct thread *t = p->threads; t; t = t->next_in_process) {
        if (t->state == THREAD_EXITED)
            continue;
        t->stop_saved_state = t->state;
        if (t->state == THREAD_RUNNABLE)
            sched_policy_remove(t);
        t->state = THREAD_STOPPED;
    }
    signal_notify_parent(p, 5, signo); /* CLD_STOPPED */
    if (p->parent)
        wakeup(p->parent);
    if (stops_self)
        schedule();
}

void proc_stop_current(int signo) {
    struct proc *p = my_proc();
    if (!p || !my_thread())
        panic("proc_stop_current without a current process");
    proc_stop(p, signo);
}

void proc_continue(struct proc *p) {
    if (!p || p->exited || !p->stopped)
        return;
    p->stopped = false;
    p->continue_event_pending = true;
    for (struct thread *t = p->threads; t; t = t->next_in_process) {
        if (t->state != THREAD_STOPPED)
            continue;
        enum thread_state saved = t->stop_saved_state;
        t->stop_saved_state = THREAD_UNUSED;
        if (saved == THREAD_SLEEPING) {
            t->state = THREAD_SLEEPING;
        } else {
            t->chan = NULL;
            t->sleep_until = 0;
            sched_policy_add(t);
        }
    }
    signal_notify_parent(p, 6, 18); /* CLD_CONTINUED, SIGCONT */
    if (p->parent)
        wakeup(p->parent);
}

/* Hand `dying`'s children to init (pid 1) so they never point at a parent
 * that is about to go away. Children that already exited become reapable
 * orphans; ones still running are reaped when they exit and a later
 * proc_reap_orphans() runs. */
static void proc_reparent_children(struct proc *dying) {
    irq_spin_lock(&proc_table_lock);
    struct proc *init = NULL;
    for (size_t i = 0; i < PROC_MAX_PROCS; i++)
        if (proc_table[i] && proc_table[i]->pid == 1 && proc_table[i] != dying)
            init = proc_table[i];
    for (size_t i = 0; i < PROC_MAX_PROCS; i++) {
        struct proc *c = proc_table[i];
        if (!c || c->parent != dying)
            continue;
        c->parent = init;       /* NULL when init itself dies: nobody to notify */
        c->orphan = true;
    }
    irq_spin_unlock(&proc_table_lock);
}

int signal_send_all(struct proc *sender, int signo) {
    struct proc *targets[PROC_MAX_PROCS];
    size_t n = 0;
    irq_spin_lock(&proc_table_lock);
    for (size_t i = 0; i < PROC_MAX_PROCS; i++) {
        struct proc *p = proc_table[i];
        if (p && p != sender && p->pid > 1 && !p->exited)
            targets[n++] = p;
    }
    irq_spin_unlock(&proc_table_lock);
    int sent = 0;
    for (size_t i = 0; i < n; i++)
        if (signal_may_send(sender, targets[i], signo) && signal_send(targets[i], signo) == 0)
            sent++;
    return sent ? 0 : -ESRCH;
}

void proc_reap_orphans(void) {
    struct proc *self = my_proc();
    struct proc *victims[PROC_MAX_PROCS];
    size_t n = 0;
    irq_spin_lock(&proc_table_lock);
    for (size_t i = 0; i < PROC_MAX_PROCS; i++) {
        struct proc *c = proc_table[i];
        if (c && c != self && c->orphan && c->exited && n < PROC_MAX_PROCS)
            victims[n++] = c;
    }
    irq_spin_unlock(&proc_table_lock);
    for (size_t i = 0; i < n; i++) {
        struct user_rusage usage;
        if (victims[i]->parent)
            resource_reap_child(victims[i]->parent, victims[i], &usage);
        proc_destroy(victims[i]);
    }
}

void proc_exit_current(int status) {
    struct proc *p = my_proc();
    struct thread *self = my_thread();
    if (!p || !self)
        panic("proc_exit_current: no current process/thread");

    proc_reap_orphans();
    proc_reparent_children(p);

    /* A fatal event in one thread kills the process: every sibling shares
     * the same potentially-corrupted address space and file descriptions. */
    proc_terminate_other_threads(p, self, false);
    p->exit_status = status;
    file_table_close_all(p);
    proc_mark_exited(p);
    fbown_check();                  /* an exiting display owner gives the console back */
    thread_set_exited(self);
    signal_notify_parent(p, status < 0 ? 2 : 1,
                         status < 0 ? -status : status);

    /* Notify exactly the direct parent. It decides whether and how to handle
     * the child status through wait(); no ancestor is skipped. */
    if (p->parent)
        wakeup(p->parent);

    schedule();

    /* Normally unreachable. If there was a transient scheduler gap, remain
     * in EL1 and let the next interrupt schedule a real runnable thread;
     * never restore the dead process's EL0 exception frame. */
    for (;;) {
        arch_wait_for_interrupt();
    }
}

/* -------------------------------------------------------------
 * Sleep / wake — same algorithm as x86's proc.c, ported directly.
 *
 * Lost-wakeup safety: proc_table_lock is held across releasing `lk`
 * and marking the caller THREAD_SLEEPING. wakeup() also needs
 * proc_table_lock, so it can't run — even from an IRQ, which masks
 * DAIF the same way irq_spin_lock does here — in the window between
 * "no longer holding lk" and "actually marked asleep", which is
 * exactly the window a racing wakeup() could otherwise find "not
 * asleep yet" and silently drop.
 * ------------------------------------------------------------- */
void sleep(void *chan, spinlock_t *lk) {
    struct thread *t = my_thread();

    irq_spin_lock(&proc_table_lock);
    irq_spin_unlock(lk);

    t->chan = chan;
    thread_set_sleeping(t);

    irq_spin_unlock(&proc_table_lock);
    schedule();

    /* --- woken --- */
    irq_spin_lock(lk);
}

void wakeup(void *chan) {
    irq_spin_lock(&proc_table_lock);
    for (size_t i = 0; i < PROC_MAX_PROCS; i++) {
        struct proc *p = proc_table[i];
        if (!p) continue;
        for (struct thread *t = p->threads; t; t = t->next_in_process)
            if (t->state == THREAD_SLEEPING && t->chan == chan) {
                t->chan = NULL;
                t->sleep_until = 0;
                sched_policy_add(t); /* sets THREAD_RUNNABLE itself */
            } else if (t->state == THREAD_STOPPED
                    && t->stop_saved_state == THREAD_SLEEPING
                    && t->chan == chan) {
                /* The event happened while the process was stopped.  Record
                 * that it should be runnable after SIGCONT without actually
                 * scheduling it before then. */
                t->chan = NULL;
                t->sleep_until = 0;
                t->stop_saved_state = THREAD_RUNNABLE;
            }
    }
    irq_spin_unlock(&proc_table_lock);
}

/* ITIMER_REAL bookkeeping. `itimers_any` lets the 1 kHz tick skip the scan
 * entirely when nothing is armed; it is recomputed by every scan. */
static bool itimers_any;
void proc_itimer_armed(void) { itimers_any = true; }

void thread_wakeup_expired(uint64_t now) {
    static struct proc *fired[PROC_MAX_PROCS];   /* IRQ context: not reentrant */
    size_t nfired = 0;
    bool scan_timers = itimers_any, any_armed = false;
    uint64_t now_ns = scan_timers ? timer_uptime_ns() : 0;
    irq_spin_lock(&proc_table_lock);
    for (size_t i = 0; i < PROC_MAX_PROCS; i++) {
        struct proc *p = proc_table[i];
        if (!p) continue;
        if (scan_timers && p->itimer_next_ns) {
            bool alive = false;
            for (struct thread *t = p->threads; t; t = t->next_in_process)
                if (t->state != THREAD_EXITED) { alive = true; break; }
            if (!alive) {
                p->itimer_next_ns = p->itimer_interval_ns = 0;
            } else if (now_ns >= p->itimer_next_ns) {
                fired[nfired++] = p;
                if (p->itimer_interval_ns) {
                    p->itimer_next_ns += p->itimer_interval_ns;
                    if (p->itimer_next_ns <= now_ns)   /* missed several periods */
                        p->itimer_next_ns = now_ns + p->itimer_interval_ns;
                    any_armed = true;
                } else {
                    p->itimer_next_ns = 0;
                }
            } else {
                any_armed = true;
            }
        }
        for (struct thread *t = p->threads; t; t = t->next_in_process)
            if (t->state == THREAD_SLEEPING && t->sleep_until
                    && now >= t->sleep_until) {
                t->chan = NULL;
                t->sleep_until = 0;
                sched_policy_add(t); /* sets THREAD_RUNNABLE itself */
            } else if (t->state == THREAD_STOPPED
                    && t->stop_saved_state == THREAD_SLEEPING
                    && t->sleep_until && now >= t->sleep_until) {
                t->chan = NULL;
                t->sleep_until = 0;
                t->stop_saved_state = THREAD_RUNNABLE;
            }
    }
    irq_spin_unlock(&proc_table_lock);
    if (scan_timers) {
        itimers_any = any_armed;
        /* signal_send() takes proc_table_lock (via wakeup), so it runs
         * after the scan, not inside it. */
        for (size_t i = 0; i < nfired; i++)
            signal_send(fired[i], SIGNAL_SIGALRM);
    }
}
