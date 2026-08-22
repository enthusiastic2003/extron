#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/kheap.h>
#include <kernel/panic.h>
#include <kernel/console.h>
#include <arch/irq_spinlock.h>
#include <stddef.h>
#include <stdbool.h>

void proc_init(struct proc *p, uint64_t pid, virt_addr_t entry,
                virt_addr_t user_sp, phys_addr_t ttbr0) {
    p->pid   = pid;
    p->state = PROC_UNUSED;
    p->ttbr0 = ttbr0;
    p->entry = entry;
    p->user_sp = user_sp;
    p->next  = NULL;
    p->chan  = NULL;
    p->sleep_until = 0;
    p->mm    = NULL;

    /* Through kmalloc (kernel/mm/kheap.c), not vmm_alloc_pages() directly
     * — kheap.c is itself just a byte-granularity layer on top of the
     * same vmm_alloc_pages()/vmm_free_pages() bitmap allocator, so this
     * is still page-backed underneath, just going through the proper
     * kernel allocator instead of reaching past it. liballoc has no
     * separate init step to report (it lazily bootstraps its first
     * block on this very call), so this print is the only direct
     * confirmation kmalloc actually ran and returned real memory,
     * rather than just inferring it from "nothing panicked". */
    p->kernel_stack_base = (virt_addr_t)kmalloc(PROC_KERNEL_STACK_PAGES * PAGE_SIZE);
    if (!p->kernel_stack_base)
        panic("aarch64 proc_init: kernel stack allocation failed");
    p->kernel_stack_top = p->kernel_stack_base + PROC_KERNEL_STACK_PAGES * PAGE_SIZE;
    kprintf("[PROC] PID %lu kernel stack: kmalloc'd %p - %p\n",
            (unsigned long)pid, (void *)p->kernel_stack_base, (void *)p->kernel_stack_top);

    /* forkret-style bootstrap: pre-populate the saved context as if this
     * proc had already been switched out once, with lr pointing at the
     * trampoline instead of a real return address, and sp at a fresh,
     * empty stack. schedule()'s first switch to this proc is then the
     * exact same context_switch() call as every other switch — see
     * proc_bootstrap_trampoline()'s comment in sched.c. */
    p->context = (struct cpu_context){0};
    p->context.lr = (uint64_t)proc_bootstrap_trampoline;
    p->context.sp = p->kernel_stack_top;
}

/* -------------------------------------------------------------
 * Process table — ported from x86's kernel/proc/proc.c
 * (~/extron-x86-backup/): a fixed slot array + a lock + a monotonic
 * PID counter. Slots aren't tied to PIDs; lookup is a linear scan —
 * fine for a hobby kernel's MAX_PROCS, switch to a hash if it ever
 * shows up in profiles (x86's own comment, still true here).
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
    kprintf("[PROC] Process table initialized (capacity %u)\n", (unsigned)MAX_PROCS);
}

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

static void proc_table_remove_locked(struct proc *p) {
    for (size_t i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i] == p) {
            proc_table[i] = NULL;
            proc_table_count--;
            return;
        }
    }
}

uint64_t proc_table_add(struct proc *p) {
    irq_spin_lock(&proc_table_lock);
    if (!proc_table_add_locked(p)) {
        irq_spin_unlock(&proc_table_lock);
        panic("proc_table_add: process table full");
    }
    uint64_t pid = next_pid++;
    irq_spin_unlock(&proc_table_lock);
    return pid;
}

void proc_table_remove(struct proc *p) {
    irq_spin_lock(&proc_table_lock);
    proc_table_remove_locked(p);
    irq_spin_unlock(&proc_table_lock);
}

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

void proc_for_each(void (*fn)(struct proc *, void *), void *arg) {
    irq_spin_lock(&proc_table_lock);
    for (size_t i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i])
            fn(proc_table[i], arg);
    }
    irq_spin_unlock(&proc_table_lock);
}

static const char *proc_state_str(enum proc_state s) {
    switch (s) {
        case PROC_UNUSED:   return "UNUSED";
        case PROC_RUNNABLE: return "RUNNABLE";
        case PROC_RUNNING:  return "RUNNING";
        case PROC_SLEEPING: return "SLEEPING";
        case PROC_ZOMBIE:   return "ZOMBIE";
        default:            return "UNKNOWN";
    }
}

void proc_dump_table(void) {
    irq_spin_lock(&proc_table_lock);

    kprintf("\n========================================\n");
    kprintf("PID STATE TTBR0 CHAN\n");
    kprintf("----------------------------------------\n");
    for (size_t i = 0; i < MAX_PROCS; i++) {
        struct proc *p = proc_table[i];
        if (!p) continue;
        kprintf("%lu %s 0x%lx 0x%lx\n",
                (unsigned long)p->pid, proc_state_str(p->state),
                (unsigned long)p->ttbr0, (unsigned long)(uint64_t)p->chan);
    }
    kprintf("----------------------------------------\n");
    kprintf("TOTAL: %lu\n========================================\n",
            (unsigned long)proc_table_count);

    irq_spin_unlock(&proc_table_lock);
}

/* -------------------------------------------------------------
 * State helpers
 * ------------------------------------------------------------- */
void proc_set_runnable(struct proc *p) { if (p) p->state = PROC_RUNNABLE; }
void proc_set_running(struct proc *p)  { if (p) p->state = PROC_RUNNING; }
void proc_set_sleeping(struct proc *p) { if (p) p->state = PROC_SLEEPING; }
void proc_set_zombie(struct proc *p)   { if (p) p->state = PROC_ZOMBIE; }

/* -------------------------------------------------------------
 * Sleep / wake — same algorithm as x86's proc.c, ported directly.
 *
 * Lost-wakeup safety: proc_table_lock is held across releasing `lk`
 * and marking the caller PROC_SLEEPING. wakeup() also needs
 * proc_table_lock, so it can't run — even from an IRQ, which masks
 * DAIF the same way irq_spin_lock does here — in the window between
 * "no longer holding lk" and "actually marked asleep", which is
 * exactly the window a racing wakeup() could otherwise find "not
 * asleep yet" and silently drop.
 * ------------------------------------------------------------- */
void sleep(void *chan, spinlock_t *lk) {
    struct proc *p = my_proc();

    irq_spin_lock(&proc_table_lock);
    irq_spin_unlock(lk);

    p->chan = chan;
    proc_set_sleeping(p);

    irq_spin_unlock(&proc_table_lock);
    schedule();

    /* --- woken --- */
    irq_spin_lock(lk);
}

void wakeup(void *chan) {
    irq_spin_lock(&proc_table_lock);
    for (size_t i = 0; i < MAX_PROCS; i++) {
        struct proc *p = proc_table[i];
        if (p && p->state == PROC_SLEEPING && p->chan == chan) {
            p->chan = NULL;
            sched_policy_add(p); /* sets PROC_RUNNABLE itself */
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
                sched_policy_add(p); /* sets PROC_RUNNABLE itself */
            }
        }
    }
    irq_spin_unlock(&proc_table_lock);
}
