#ifndef KERNEL_PROC_PROC_H
#define KERNEL_PROC_PROC_H

#include <stdint.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/sync/spinlock.h>

struct vm_space; /* kernel/mm/uvm.h — forward-declared to avoid a circular include */

/*
 * aarch64 process control block. Deliberately no thread split (yet) —
 * threading is out of scope until fork/syscalls/mlibc integration lands;
 * one struct proc is one address space with exactly one execution
 * context.
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

enum proc_state {
    PROC_UNUSED = 0,
    PROC_RUNNABLE,
    PROC_RUNNING,
    PROC_SLEEPING,
    PROC_ZOMBIE
};

/*
 * aarch64 callee-saved register set (AAPCS64: x19-x28, fp, lr, sp) —
 * saved/restored by context_switch (kernel/arch/aarch64/proc/switch.S).
 * Field order must match that file's hardcoded offsets:
 *   x19..x28 = 0x00..0x48 (8 regs, 8 bytes each)
 *   fp = 0x50   lr = 0x58   sp = 0x60
 */
struct cpu_context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t fp;
    uint64_t lr;
    uint64_t sp;
};

struct proc {
    uint64_t            pid;
    enum proc_state     state;
    phys_addr_t         ttbr0;              /* create_user_pml4() */
    struct cpu_context  context;
    virt_addr_t         kernel_stack_base;
    virt_addr_t         kernel_stack_top;
    virt_addr_t         entry;               /* EL0 entry point, used once on first launch */
    virt_addr_t         user_sp;             /* EL0 initial SP_EL0, used once on first launch */
    struct proc         *next;               /* run-queue link */

    void                *chan;               /* wait channel, valid while PROC_SLEEPING */
    uint64_t            sleep_until;         /* wake when timer_ticks() >= this; 0 = not timed */
    struct vm_space     *mm;                 /* user address-space allocator (kernel/mm/uvm.c) */
};

/* Size of the per-process kernel stack (interrupts land here). Smaller
 * than x86's PROC_KERNEL_STACK_PAGES (8 / 32KB) — no syscalls/deep
 * kernel call chains yet, just the exception-vector path. */
#define PROC_KERNEL_STACK_PAGES 4

/* Initializes a caller-allocated struct proc in place (no kmalloc/heap
 * dependency yet — this milestone's two test processes are static
 * globals; dynamic allocation is future work alongside fork()).
 * Allocates a kernel stack via vmm_alloc_pages() and pre-populates
 * context.lr/context.sp so schedule()'s first switch to this proc lands
 * in proc_bootstrap_trampoline() (kernel/proc/sched.c) instead
 * of needing a special case for "never run before". */
void proc_init(struct proc *p, uint64_t pid, virt_addr_t entry,
                virt_addr_t user_sp, phys_addr_t ttbr0);

/* ---------------------------------------------------------------
 * Process table — every proc from proc_table_add() until
 * proc_table_remove(). Ported from x86's kernel/proc/proc.c
 * (~/extron-x86-backup/): a fixed slot array + a lock + a monotonic
 * PID counter, nothing here touches a register or instruction. Distinct
 * from the scheduler's run queue (kernel/proc/sched_policy_rr.c), which
 * only holds RUNNABLE, off-CPU procs — this table holds everything
 * alive regardless of state.
 * --------------------------------------------------------------- */
void          proc_table_init(void);
uint64_t      proc_table_add(struct proc *p);    /* assigns and returns p->pid */
void          proc_table_remove(struct proc *p);
struct proc  *proc_lookup(uint64_t pid);
void          proc_for_each(void (*fn)(struct proc *, void *), void *arg);
void          proc_dump_table(void);

/* ---------------------------------------------------------------
 * State helpers
 * --------------------------------------------------------------- */
void proc_set_runnable(struct proc *p);
void proc_set_running(struct proc *p);
void proc_set_sleeping(struct proc *p);
void proc_set_zombie(struct proc *p);

/* ---------------------------------------------------------------
 * Sleep / wake — also ported from x86's proc.c, same algorithm
 * (including the lost-wakeup guard: proc_table_lock is held across
 * releasing `lk` and marking the caller PROC_SLEEPING, which is what
 * stops a wakeup() racing in via an IRQ during that exact window from
 * finding "not asleep yet" and silently doing nothing).
 * --------------------------------------------------------------- */
void sleep(void *chan, spinlock_t *lk);   /* caller holds lk; returns with lk re-held */
void wakeup(void *chan);                   /* wakes every sleeper on chan; IRQ-safe */
void proc_wakeup_expired(uint64_t now);    /* wakes timed sleepers whose deadline passed */

#endif
