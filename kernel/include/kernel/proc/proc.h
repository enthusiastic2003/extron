#ifndef KERNEL_PROC_PROC_H
#define KERNEL_PROC_PROC_H

#include <stdint.h>
#include <stdbool.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/sync/spinlock.h>
#include <kernel/fs/file.h>
#include <kernel/proc/signal.h>
#include <kernel/proc/resource.h>

struct vm_space; /* kernel/mm/uvm.h — forward-declared to avoid a circular include */
struct proc;

/*
 * A process owns resources shared by its threads; a thread is the unit the
 * scheduler runs. Each process embeds its main thread and links dynamically
 * allocated workers created by the clone-style thread syscall.
 *
 * This aarch64 tree (kernel/) is now a standalone project, backed up
 * from x86's original kernel/ under backup/x86_tree/ — see that
 * commit's message for why. struct proc here is descended from, but no
 * longer needs to coexist with, x86's own differently-shaped struct
 * proc (fs_base/tf/mm/chan/sleep_until, CR3 instead of TTBR0) — that's
 * why this now lives at the same kernel/proc/ path x86 used, instead of
 * under arch/aarch64/ where it was forced to sit while both trees were
 * still compiled from the same repo.
 */

enum thread_state {
    THREAD_UNUSED = 0,
    THREAD_RUNNABLE,
    THREAD_RUNNING,
    THREAD_SLEEPING,
    THREAD_STOPPED,
    THREAD_EXITED,
};

/*
 * aarch64 callee-saved register set (AAPCS64: x19-x28, fp, lr, sp) —
 * saved/restored by context_switch (kernel/arch/aarch64/proc/switch.S).
 * Field order must match that file's hardcoded offsets:
 *   x19..x28 = 0x00..0x48 (8 regs, 8 bytes each)
 *   fp = 0x50   lr = 0x58   sp = 0x60
 */
/*
 * Saved by context_switch (kernel/arch/aarch64/proc/switch.S). Field
 * order and offsets are load-bearing — that file hardcodes them.
 *
 * The FP/SIMD half is not the usual AAPCS64 callee-saved subset
 * (d8-d15). It is ALL of v0-v31, plus FPCR/FPSR, because what's being
 * preserved here isn't a compiler calling convention — it's a user
 * process's live floating-point state, and a process gets preempted at
 * an arbitrary instruction, not at a call boundary.
 *
 * That works because the kernel is built -mgeneral-regs-only (see the
 * Makefile's comment), so it provably never touches FP. Exception entry
 * doesn't save v0-v31 and doesn't need to: the registers still hold the
 * interrupted process's own values all the way down to here, which is
 * exactly what makes this the right place to swap them.
 *
 * Aligned to 16 so the q-register stp/ldp pairs land on aligned
 * addresses. Not strictly required — boot.S leaves SCTLR_EL1.A clear,
 * so unaligned SIMD accesses are permitted — but free to get right.
 */
struct cpu_context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28; /* 0x00 */
    uint64_t fp;                                               /* 0x50 */
    uint64_t lr;                                               /* 0x58 */
    uint64_t sp;                                               /* 0x60 */
    uint64_t fpcr;                                             /* 0x68 */
    uint64_t fpsr;                                             /* 0x70 */
    /* TPIDR_EL0 — the thread pointer. Architecturally EL0-writable
     * (that's why sys_tcb_set() notes it may never be called), and
     * per-thread by definition, so it belongs here for exactly the same
     * reason the FP registers do: the kernel never touches it, so its
     * value at this point is still the outgoing process's own.
     *
     * Nothing uses TLS yet, which is the only reason this wasn't already
     * a bug — the same way FP/SIMD and SP_EL0 were both invisible until
     * a workload happened to use the register. Fixed ahead of mlibc
     * rather than after it, since the symptom would be every thread
     * seeing another process's TLS base.
     *
     * Sits in what used to be pure alignment padding, so the struct's
     * size and v[]'s offset are unchanged. */
    uint64_t tpidr_el0;                   /* 0x78 */
    uint64_t v[64];                       /* 0x80: v0-v31, 128 bits each */
} __attribute__((aligned(16)));

struct thread {
    uint64_t            tid;
    enum thread_state   state;
    struct proc         *process;
    struct cpu_context  context;
    virt_addr_t         kernel_stack_base;
    virt_addr_t         kernel_stack_top;
    virt_addr_t         entry;               /* EL0 entry point, used once on first launch */
    virt_addr_t         user_sp;             /* EL0 initial SP_EL0, used once on first launch */
    void                *chan;               /* wait channel, valid while THREAD_SLEEPING */
    uint64_t            sleep_until;         /* wake when timer_ticks() >= this; 0 = not timed */
    enum thread_state   stop_saved_state;    /* state to restore after process continuation */
    virt_addr_t         exit_word;           /* userspace completion word, set to 1 at exit */
    uint64_t            signal_mask;         /* blocked signals, bit (signo - 1) */
    bool                wait_mask_restore;   /* pselect mask is live until delivery */
    uint64_t            wait_original_mask;  /* mask restored by signal return */
    uint64_t            signal_pending;      /* thread-directed pending signals */
    struct signal_info  signal_info[SIGNAL_MAX + 1];
    uint64_t            cpu_account_start_ns;
    enum resource_cpu_mode cpu_account_mode;
    struct thread       *next_in_process;
};

struct proc {
    uint64_t            pid;
    phys_addr_t         ttbr0;              /* create_user_pml4() */
    struct vm_space     *mm;                 /* user address-space allocator (kernel/mm/uvm.c) */
    struct thread       main_thread;
    struct thread       *threads;
    size_t              thread_count;

    /* argc/argv for the process's first instruction, passed in x0/x1 by
     * proc_bootstrap_trampoline() the same way any AAPCS64 caller would
     * pass them. Only meaningful up to first launch; execve() rewrites
     * the trap frame directly rather than going through here. Before
     * this existed, main() received whatever happened to be in x0/x1 —
     * usr/lib/crt0.S zeroed them itself to paper over it. */
    uint64_t            user_argc;
    virt_addr_t         user_argv;

    /* Who gets to wait() for this one, and what it left behind. NULL
     * parent = created by the kernel at boot, so nothing will ever reap
     * it — see sys_wait() on why that is a leak and not a crash. */
    struct proc         *parent;
    uint64_t            pgid;              /* process group for job control */
    uint64_t            sid;               /* session containing that group */
    spinlock_t          cred_lock;
    uint32_t            ruid, euid, suid;
    uint32_t            rgid, egid, sgid;
    uint32_t            supplementary_groups[VFS_GROUP_MAX];
    size_t              supplementary_group_count;
    uint32_t            file_umask;
    spinlock_t          resource_lock;
    struct proc_rlimit  resource_limits[PROC_RLIMIT_COUNT];
    uint64_t            cpu_user_ns;
    uint64_t            cpu_system_ns;
    uint64_t            children_user_ns;
    uint64_t            children_system_ns;
    int                 exit_status;
    bool                exited;
    bool                stopped;
    int                 stop_signal;
    bool                stop_event_pending;
    bool                continue_event_pending;
    spinlock_t          signal_lock;
    uint64_t            signal_pending;
    struct signal_info  signal_info[SIGNAL_MAX + 1];
    struct signal_action signal_actions[SIGNAL_MAX + 1];
    struct open_file    *files[PROC_MAX_FDS];
    uint8_t             fd_flags[PROC_MAX_FDS];
    spinlock_t          cwd_lock;
    struct vfs_path     cwd;                 /* retained directory location */
};

/* Size of a per-thread kernel stack (interrupts land here). Was 4
 * pages on the reasoning that there were "no syscalls/deep kernel call
 * chains yet, just the exception-vector path" — that stopped being true
 * some time ago. Syscalls run on this stack, kbd_getc() sleeps partway
 * down one, and install_and_switch() now puts a 640-byte struct
 * cpu_context scratch on it. Matches x86's 8 pages again.
 *
 * Allocated with one extra page below it, left unmapped as a guard —
 * see proc_init(). Overflow then takes a clean Data Abort naming the
 * faulting address instead of silently corrupting whatever the
 * allocator happened to place underneath, which is the failure mode
 * that cost real debugging time on the stale-object-file bug. */
#define THREAD_KERNEL_STACK_PAGES 8

/* Initializes a caller-allocated struct proc in place (no kmalloc/heap
 * dependency yet — this milestone's two test processes are static
 * globals; dynamic allocation is future work alongside fork()).
 * Initializes the embedded main thread, allocates its guarded kernel stack,
 * and pre-populates context.lr/context.sp so the first switch lands
 * in proc_bootstrap_trampoline() (kernel/proc/sched.c) instead
 * of needing a special case for "never run before". */
void proc_init(struct proc *p, uint64_t pid, virt_addr_t entry,
                virt_addr_t user_sp, phys_addr_t ttbr0);

/* Releases everything proc_init() acquired plus the address space, and
 * removes the proc from the table. Must not run on the currently
 * executing proc — it frees the kernel stack underfoot and the page
 * tables TTBR0_EL1 points at. sys_wait() calls it from the PARENT's
 * context for exactly that reason. */
void proc_destroy(struct proc *p);

/* A process's cwd is shared by all of its threads. Callers use snapshots so
 * a concurrent chdir cannot release the dentry during pathname traversal. */
int  proc_cwd_snapshot(struct proc *p, struct vfs_path *out);
void proc_cwd_set(struct proc *p, const struct vfs_path *path);

/* fork(): a duplicate of `parent` that resumes from the trap frame `f`
 * with x0 == 0, already on the run queue. NULL on failure, having
 * changed nothing about the parent. kernel/proc/proc_fork.c. */
struct aarch64_frame;
struct proc *proc_fork(struct proc *parent, struct aarch64_frame *f);

/* ---------------------------------------------------------------
 * Process table — every proc from proc_table_add() until
 * proc_table_remove(). Ported from x86's kernel/proc/proc.c
 * (~/extron-x86-backup/): a fixed slot array + a lock + a monotonic
 * PID counter, nothing here touches a register or instruction. Distinct
 * from the scheduler's run queue (kernel/proc/sched_policy_rr.c), which
 * holds RUNNABLE, off-CPU threads — this table owns every process and
 * makes each process's thread list discoverable for wakeups.
 * --------------------------------------------------------------- */
void          proc_table_init(void);
uint64_t      proc_alloc_pid(void);
uint64_t      proc_alloc_tid(void);
void          proc_table_add(struct proc *p);    /* publishes a fully initialized process */
void          proc_table_remove(struct proc *p);
struct proc  *proc_lookup(uint64_t pid);
void          proc_for_each(void (*fn)(struct proc *, void *), void *arg);
void          proc_dump_table(void);

/* sys_wait()'s scan: a reapable (ZOMBIE) child of `parent`, or NULL.
 * *out_any_children reports whether it has any children at all, which
 * is how wait() tells "not yet" from "never". */
struct proc  *proc_find_zombie_child(struct proc *parent, bool *out_any_children);
struct proc  *proc_find_waitable_child(struct proc *parent, int64_t selector,
                                       int options, int *event_status,
                                       bool *out_any_children);
bool          proc_group_exists(uint64_t pgid, uint64_t sid);
void          proc_vfs_cred_snapshot(struct proc *p, struct vfs_cred *out);
void          proc_vfs_real_cred_snapshot(struct proc *p, struct vfs_cred *out);
uint32_t      proc_get_umask(struct proc *p);
uint32_t      proc_set_umask(struct proc *p, uint32_t mask);

/* ---------------------------------------------------------------
 * Thread state and process-exit helpers
 * --------------------------------------------------------------- */
void thread_set_runnable(struct thread *t);
void thread_set_running(struct thread *t);
void thread_set_sleeping(struct thread *t);
void thread_set_exited(struct thread *t);
void proc_mark_exited(struct proc *p);
void proc_stop(struct proc *p, int signo);
void proc_stop_current(int signo);
void proc_continue(struct proc *p);

/* Terminate the entire current process, wake only its direct parent, and
 * schedule away permanently. Non-negative status is a normal exit code;
 * negative status means termination by the corresponding signal number. */
void proc_exit_current(int status) __attribute__((noreturn));

/* Create another schedulable thread in p's existing address space. The
 * caller has already validated the userspace entry, stack and TLS values.
 * The returned object is linked into p and queued runnable. */
struct thread *proc_thread_create(struct proc *p, virt_addr_t entry,
                                  virt_addr_t user_sp, uint64_t tls,
                                  virt_addr_t exit_word);

/* Find/reap a thread belonging to p. Reaping only succeeds after exit and
 * never frees the embedded main-thread object. */
struct thread *proc_thread_lookup(struct proc *p, uint64_t tid);
int            proc_thread_reap(struct proc *p, uint64_t tid);

/* Used by process exit and successful exec. Every sibling is taken off the
 * run queue; exec additionally destroys its kernel object immediately. */
void proc_terminate_other_threads(struct proc *p, struct thread *self,
                                  bool reap);

/* True when no thread other than t remains capable of running. */
bool proc_thread_is_last_live(struct proc *p, struct thread *t);

/* ---------------------------------------------------------------
 * Sleep / wake — also ported from x86's proc.c, same algorithm
 * (including the lost-wakeup guard: proc_table_lock is held across
 * releasing `lk` and marking the caller THREAD_SLEEPING, which is what
 * stops a wakeup() racing in via an IRQ during that exact window from
 * finding "not asleep yet" and silently doing nothing).
 * --------------------------------------------------------------- */
void sleep(void *chan, spinlock_t *lk);   /* caller holds lk; returns with lk re-held */
void wakeup(void *chan);                   /* wakes every sleeper on chan; IRQ-safe */
void thread_wakeup_expired(uint64_t now);  /* wakes timed sleepers whose deadline passed */

#endif
