#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/kheap.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/pmm.h>
#include <kernel/panic.h>
#include <kernel/console.h>
#include <kernel/mm/uvm.h>
#include <kernel/fs/file.h>
#include <arch/irq_spinlock.h>
#include <stddef.h>
#include <stdbool.h>

/* kernel/arch/aarch64/proc/switch.S hardcodes these offsets. A silent
 * mismatch would corrupt FP state across every context switch in a way
 * that only shows up as wrong pixels or wrong arithmetic much later, so
 * fail the build instead. */
_Static_assert(offsetof(struct cpu_context, sp)   == 0x60, "switch.S: sp offset");
_Static_assert(offsetof(struct cpu_context, fpcr) == 0x68, "switch.S: fpcr offset");
_Static_assert(offsetof(struct cpu_context, fpsr) == 0x70, "switch.S: fpsr offset");
_Static_assert(offsetof(struct cpu_context, tpidr_el0) == 0x78, "switch.S: tpidr_el0 offset");
_Static_assert(offsetof(struct cpu_context, v)    == 0x80, "switch.S: v[] offset");
_Static_assert(sizeof(((struct cpu_context *)0)->v) == 32 * 16, "switch.S: v[] covers v0-v31");

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
    p->user_argc = 0;
    p->user_argv = 0;
    p->parent = NULL;
    p->exit_status = 0;
    p->cwd[0] = '\0';
    file_table_init(p);

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
    virt_addr_t region = vmm_alloc_pages(PROC_KERNEL_STACK_PAGES + 1);
    if (!region)
        panic("aarch64 proc_init: kernel stack allocation failed");

    phys_addr_t guard_phys = kvirt_to_phys(region);
    kunmap(region);
    if (guard_phys)
        pmm_free_page((void *)guard_phys);

    p->kernel_stack_base = region + PAGE_SIZE;
    p->kernel_stack_top  = region + (PROC_KERNEL_STACK_PAGES + 1) * PAGE_SIZE;
    kprintf("[PROC] PID %lu kernel stack: %p - %p (guard page at %p)\n",
            (unsigned long)pid, (void *)p->kernel_stack_base,
            (void *)p->kernel_stack_top, (void *)region);

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
    if (p->mm) {
        vm_space_destroy(p->mm);   /* pages, then the tables under them */
        p->mm = NULL;
        p->ttbr0 = 0;
    }

    /* The guard page sits one page below the base and is unmapped by
     * construction, so it can't go through vmm_free_pages(). */
    virt_addr_t guard = p->kernel_stack_base - PAGE_SIZE;
    vmm_free_pages(p->kernel_stack_base, PROC_KERNEL_STACK_PAGES);
    vmm_free_unmapped_page(guard);

    proc_table_remove(p);
    kfree(p);
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
/* PIDs start at 1, not 0. fork() reports 0 to the child and the child's
 * pid to the parent, so a process that legitimately owned pid 0 would
 * make those two answers indistinguishable. */
static uint64_t     next_pid         = 1;

void proc_table_init(void) {
    irq_spin_lock(&proc_table_lock);
    for (size_t i = 0; i < MAX_PROCS; i++)
        proc_table[i] = NULL;
    proc_table_count = 0;
    next_pid         = 1;
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
    for (size_t i = 0; i < MAX_PROCS; i++) {
        struct proc *p = proc_table[i];
        if (!p || p->parent != parent)
            continue;
        any = true;
        if (p->state == PROC_ZOMBIE) {
            found = p;
            break;
        }
    }
    irq_spin_unlock(&proc_table_lock);

    if (out_any_children)
        *out_any_children = any;
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
    kprintf("TOTAL: %lu  PMM FREE: %lu pages\n========================================\n",
            (unsigned long)proc_table_count,
            (unsigned long)pmm_free_pages());

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
            p->sleep_until = 0;
            sched_policy_add(p); /* sets PROC_RUNNABLE itself */
        }
    }
    irq_spin_unlock(&proc_table_lock);
}

void proc_wakeup_expired(uint64_t now) {
    irq_spin_lock(&proc_table_lock);
    for (size_t i = 0; i < MAX_PROCS; i++) {
        struct proc *p = proc_table[i];
        if (p && p->state == PROC_SLEEPING && p->sleep_until
                && now >= p->sleep_until) {
            p->chan = NULL;
            p->sleep_until = 0;
            sched_policy_add(p); /* sets PROC_RUNNABLE itself */
        }
    }
    irq_spin_unlock(&proc_table_lock);
}
