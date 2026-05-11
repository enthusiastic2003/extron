#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>          /* for sched_remove() */
#include <kernel/sync/spinlock.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/kheap.h>
#include <kernel/proc/elf_loader.h>
#include <kernel/fs/tar.h>
#include <kernel/console.h>
#include <kernel/klibc/string.h>
#include <kernel/proc/exec.h>

#include <kernel/proc/proc.h>

#include <stdbool.h>

/* -------------------------------------------------------------
 * Process table — every proc from proc_alloc() until proc_destroy()
 *
 * Distinct from the scheduler's run queue. The run queue holds
 * only RUNNABLE, off-CPU processes; this table holds everything
 * alive regardless of state — running, runnable, sleeping, zombie.
 *
 * Slots are not tied to PIDs. PIDs stay monotonic via next_pid;
 * lookup is a linear scan. For a hobby kernel with modest MAX_PROCS
 * that is fine; switch to a hash if it ever shows up in profiles.
 *
 * 
 * Locking:
 *   - proc_table_lock protects proc_table[], proc_table_count, next_pid.
 *   - If both proc_table_lock and run_queue_lock are ever held
 *     simultaneously, acquire proc_table_lock FIRST.  proc_destroy()
 *     does not nest them — it drops one before taking the other —
 *     so the rule is for future code.
 * ------------------------------------------------------------- */
#define MAX_PROCS 256

static struct proc *proc_table[MAX_PROCS];
static size_t       proc_table_count = 0;
static spinlock_t   proc_table_lock  = SPINLOCK_INIT;
static uint64_t     next_pid         = 0;

void proc_table_init(void) {
    irq_spin_lock(&proc_table_lock);
    for (size_t i = 0; i < MAX_PROCS; i++)
        proc_table[i] = NULL;
    proc_table_count = 0;
    next_pid         = 0;
    irq_spin_unlock(&proc_table_lock);
    kprintf("[PROC] Process table initialized (capacity %u)\n",
            (unsigned)MAX_PROCS);
}

/* Insert into the first free slot. Returns false if the table is full.
   Caller holds proc_table_lock. */
static bool proc_table_add_locked(struct proc *p) {
    for (size_t i = 0; i < MAX_PROCS; i++) {
        if (!proc_table[i]) {
            proc_table[i] = p;
            proc_table_count++;
            return true;
        }
    }
    return false;
}

/* Caller holds proc_table_lock. */
static void proc_table_remove_locked(struct proc *p) {
    for (size_t i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i] == p) {
            proc_table[i] = NULL;
            proc_table_count--;
            return;
        }
    }
}

/* -------------------------------------------------------------
 * proc_lookup — find a process by PID
 *
 * Returns the proc pointer or NULL.  The pointer is only safe
 * while the caller can guarantee the proc won't be destroyed
 * concurrently.  On a single core with non-preemptive kernel
 * critical sections, briefly holding the result is fine.  Once
 * SMP or kernel preemption lands, this will need refcounting
 * or callers must hold a higher-level lock across use.
 * ------------------------------------------------------------- */
struct proc *proc_lookup(uint64_t pid) {
    irq_spin_lock(&proc_table_lock);
    struct proc *found = NULL;
    for (size_t i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i] && proc_table[i]->pid == pid) {
            found = proc_table[i];
            break;
        }
    }
    irq_spin_unlock(&proc_table_lock);
    return found;
}

/* -------------------------------------------------------------
 * proc_for_each — invoke fn on every live process
 *
 * The callback runs WITH proc_table_lock held.  It must not
 * block, must not call proc_alloc / proc_destroy, and must not
 * acquire run_queue_lock (lock-ordering rule).  Intended for
 * read-only enumeration: ps, signal broadcast, debug dumps.
 * ------------------------------------------------------------- */
void proc_for_each(void (*fn)(struct proc *, void *), void *arg) {
    irq_spin_lock(&proc_table_lock);
    for (size_t i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i])
            fn(proc_table[i], arg);
    }
    irq_spin_unlock(&proc_table_lock);
}

/* -------------------------------------------------------------
 * Process allocation
 * ------------------------------------------------------------- */
struct proc *proc_alloc(struct proc *parent) {
    struct proc *p = kmalloc(sizeof(struct proc));
    if (!p)
        return NULL;

    memset(p, 0, sizeof(*p));

    /* Reserve a slot and assign PID atomically */
    irq_spin_lock(&proc_table_lock);
    if (!proc_table_add_locked(p)) {
        irq_spin_unlock(&proc_table_lock);
        kprintf("[PROC] Process table full, cannot allocate\n");
        kfree(p);
        return NULL;
    }
    p->pid = next_pid++;
    irq_spin_unlock(&proc_table_lock);

    p->state = PROC_UNUSED;

    p->kernel_stack_base =
        vmm_alloc_pages(PROC_KERNEL_STACK_PAGES);
    p->kernel_rsp =
        p->kernel_stack_base +
        PROC_KERNEL_STACK_PAGES * PAGE_SIZE;
    p->kernel_stack_top = p->kernel_rsp;

    if (parent) {
        p->parent = parent;
    } else {
        p->parent = NULL;
    }

    return p;
}

/* -------------------------------------------------------------
 * Free low-level proc resources
 * ------------------------------------------------------------- */
void proc_free(struct proc *p) {
    if (!p)
        return;
    if (p->kernel_stack_base) {
        vmm_free_pages(
            p->kernel_stack_base,
            PROC_KERNEL_STACK_PAGES
        );
    }
    if (p->mm) {
        vm_space_destroy(p->mm);
        p->mm = NULL;
    }
    kfree(p);
}

/* -------------------------------------------------------------
 * Destroy process
 * ------------------------------------------------------------- */
void proc_destroy(struct proc *p) {
    if (!p)
        return;
    /*
     * TODO:
     * - destroy userspace address space
     * - free page tables
     * - close files
     * - release cwd
     * - notify parent/waiters
     */
    proc_set_zombie(p);

    /* Drop from the scheduler's run queue first so no future
       schedule() can dereference this pointer. */
    sched_remove(p);

    /* Then remove from the global process table so PID lookups
       can no longer hand it out. */
    irq_spin_lock(&proc_table_lock);
    proc_table_remove_locked(p);
    irq_spin_unlock(&proc_table_lock);

    proc_free(p);
}

/* -------------------------------------------------------------
 * State helpers
 * ------------------------------------------------------------- */
void proc_set_runnable(struct proc *p) {
    if (!p)
        return;
    p->state = PROC_RUNNABLE;
}

void proc_set_running(struct proc *p) {
    if (!p)
        return;
    p->state = PROC_RUNNING;
}

void proc_set_sleeping(struct proc *p) {
    if (!p)
        return;
    p->state = PROC_SLEEPING;
}

void proc_set_zombie(struct proc *p) {
    if (!p)
        return;
    p->state = PROC_ZOMBIE;
}


/*
* 
*
* SLEEP/WAKE APIs
*
*/

/* -------------------------------------------------------------
 * Sleep / Wakeup Synchronization
 * ------------------------------------------------------------- */

/* sleep — atomically release `lk` and block on `chan`.
 *
 * Caller holds `lk` and has re-checked the condition under it.
 * Returns with `lk` re-acquired.
 *
 * Lost-wakeup safety: proc_table_lock is held across the
 * lk-release / state-change sequence; wakeup() takes the same
 * lock, so it cannot run between us releasing lk and us being
 * marked SLEEPING.
 *
 * Process context only.
 */
void sleep(void *chan, spinlock_t *lk) {
    struct proc *p = my_cpu();
    // KASSERT(p != NULL);
    // KASSERT(p->state == PROC_RUNNING);
    //  kprintf("We are in sleep\n");
    irq_spin_lock(&proc_table_lock);
    irq_spin_unlock(lk);

    p->chan = chan;
    proc_set_sleeping(p);
    // kprintf("We are in going into schedule\n");

    irq_spin_unlock(&proc_table_lock);
    schedule();
    // kprintf("Returned from schedule()\n");

    /* --- woken --- */
    irq_spin_lock(lk);
}


/* wakeup — wake every process sleeping on `chan`.
 *
 * Safe from IRQ context. Lock ordering:
 *   proc_table_lock -> run_queue_lock (via sched_add).
 */
void wakeup(void *chan) {
    irq_spin_lock(&proc_table_lock);
    for (size_t i = 0; i < MAX_PROCS; i++) {
        struct proc *p = proc_table[i];
        if (p && p->state == PROC_SLEEPING && p->chan == chan) {
            p->chan = NULL;
            proc_set_runnable(p);
            sched_add(p);
        }
    }
    irq_spin_unlock(&proc_table_lock);
}

void proc_wakeup_expired(uint64_t now) {
    irq_spin_lock(&proc_table_lock);
    for (size_t i = 0; i < MAX_PROCS; i++) {
        struct proc *p = proc_table[i];
        if (p && p->state == PROC_SLEEPING && p->chan == NULL) {
            if (now >= p->sleep_until) {
                p->sleep_until = 0;
                proc_set_runnable(p);
                sched_add(p);
            }
        }
    }
    irq_spin_unlock(&proc_table_lock);
}

void proc_dump_table(void) {
    irq_spin_lock(&proc_table_lock);

    kprintf("\n");
    kprintf("========================================\n");
    kprintf("PID STATE CR3 CHAN\n");
    kprintf("----------------------------------------\n");

    for (size_t i = 0; i < MAX_PROCS; i++) {
        struct proc *p = proc_table[i];
        if (!p)
            continue;

        kprintf("%llu ", p->pid);
        kprintf("%s ", proc_state_str(p->state));
        kprintf("0x%llx ", p->cr3);
        kprintf("0x%llx\n", (uint64_t)p->chan);
    }

    kprintf("----------------------------------------\n");
    kprintf("TOTAL: %llu\n", proc_table_count);
    kprintf("========================================\n");

    irq_spin_unlock(&proc_table_lock);
}