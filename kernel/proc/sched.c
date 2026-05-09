#include <kernel/proc/sched.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/exec.h>
#include <kernel/mm/paging.h>
#include <kernel/sync/spinlock.h>   /* adjust path if spinlock.h lives elsewhere */
#include <kernel/console.h>
#include <arch/tss.h>
#include <arch/isr.h>
#include <stddef.h>

/* ---------------------------------------------------------------
 * The raw current-process pointer.
 *
 * NOT static — the linker symbol must be visible to
 * syscall_entry.asm ("extern current_proc").
 * C code should call my_cpu() instead of touching this.
 * --------------------------------------------------------------- */
struct proc *current_proc = NULL;

/* ---------------------------------------------------------------
 * Run queue — array of struct proc pointers
 *
 * Entries [0 .. run_queue_count) are live. Removal compacts by
 * shifting trailing entries down so the array stays dense.
 * Protected by run_queue_lock.
 * --------------------------------------------------------------- */
#define RUN_QUEUE_CAPACITY 256

static struct proc *run_queue[RUN_QUEUE_CAPACITY];
static size_t       run_queue_count = 0;
static spinlock_t   run_queue_lock  = SPINLOCK_INIT;

/* ---------------------------------------------------------------
 * my_cpu — accessor for the currently running process
 * --------------------------------------------------------------- */
struct proc *my_cpu(void) {
    return current_proc;
}

/* ---------------------------------------------------------------
 * sched_init
 * --------------------------------------------------------------- */
void sched_init(void) {
    irq_spin_lock(&run_queue_lock);
    for (size_t i = 0; i < RUN_QUEUE_CAPACITY; i++)
        run_queue[i] = NULL;
    run_queue_count = 0;
    current_proc    = NULL;
    irq_spin_unlock(&run_queue_lock);
    kprintf("[SCHED] Scheduler initialized\n");
}

/* ---------------------------------------------------------------
 * sched_add — append a RUNNABLE process to the run queue
 * --------------------------------------------------------------- */
void sched_add(struct proc *p) {
    if (!p)
        return;

    irq_spin_lock(&run_queue_lock);
    if (run_queue_count >= RUN_QUEUE_CAPACITY) {
        irq_spin_unlock(&run_queue_lock);
        kprintf("[SCHED] Run queue full, cannot add PID %llu\n", p->pid);
        return;
    }
    run_queue[run_queue_count++] = p;
    irq_spin_unlock(&run_queue_lock);
}

/* ---------------------------------------------------------------
 * sched_remove — remove a process from the run queue
 * --------------------------------------------------------------- */
void sched_remove(struct proc *p) {
    if (!p)
        return;

    irq_spin_lock(&run_queue_lock);
    for (size_t i = 0; i < run_queue_count; i++) {
        if (run_queue[i] == p) {
            /* Shift trailing entries down to keep the array compact */
            for (size_t j = i; j + 1 < run_queue_count; j++)
                run_queue[j] = run_queue[j + 1];
            run_queue_count--;
            run_queue[run_queue_count] = NULL;
            break;
        }
    }
    irq_spin_unlock(&run_queue_lock);
}

/* ---------------------------------------------------------------
 * schedule — pick the next runnable process and context-switch
 *
 * Called from:
 *   - timer interrupt handler (preemption)
 *   - voluntary yield (future)
 * --------------------------------------------------------------- */
void schedule(void) {
    struct proc *old = current_proc;
    if (!old)
        return;

    irq_spin_lock(&run_queue_lock);

    struct proc *next     = NULL;
    size_t       next_idx = 0;
    for (size_t i = 0; i < run_queue_count; i++) {
        if (run_queue[i]->state == PROC_RUNNABLE) {
            next     = run_queue[i];
            next_idx = i;
            break;
        }
    }

    if (!next) {
        irq_spin_unlock(&run_queue_lock);

        if (old->state == PROC_RUNNING)
            return;

        /* Idle until an IRQ produces a runnable. sti+hlt is atomic
           on x86: no IRQ lands between them. cli after hlt restores
           the disabled state the rest of schedule() expects. */
        for (;;) {
            __asm__ volatile ("sti; hlt; cli");

            irq_spin_lock(&run_queue_lock);
            int any = 0;
            for (size_t i = 0; i < run_queue_count; i++) {
                if (run_queue[i]->state == PROC_RUNNABLE) { any = 1; break; }
            }
            irq_spin_unlock(&run_queue_lock);

            if (any)
                break;
        }
        return schedule();
    }

    /* Compact next out of the queue */
    for (size_t j = next_idx; j + 1 < run_queue_count; j++)
        run_queue[j] = run_queue[j + 1];
    run_queue_count--;
    run_queue[run_queue_count] = NULL;

    if (old->state == PROC_RUNNING) {
        old->state = PROC_RUNNABLE;
        if (run_queue_count < RUN_QUEUE_CAPACITY)
            run_queue[run_queue_count++] = old;
        else
            kprintf("[SCHED] Run queue full, dropping PID %llu\n", old->pid);
    }

    irq_spin_unlock(&run_queue_lock);

    next->state  = PROC_RUNNING;
    current_proc = next;

    tss_set_rsp0(next->kernel_stack_top);
    if (old->cr3 != next->cr3)
        load_cr3(next->cr3);

    context_switch(&old->context, &next->context);
}

/* ---------------------------------------------------------------
 * sched_start — launch the very first user process
 *
 * Called once from kernel_stage2. Never returns.
 *
 * The process was prepared by create_init_proc(), which placed
 * a trap frame at the top of its kernel stack. We use
 * enter_userspace() to jump into ring 3.
 * --------------------------------------------------------------- */
void sched_start(void) {
    irq_spin_lock(&run_queue_lock);

    if (run_queue_count == 0) {
        irq_spin_unlock(&run_queue_lock);
        kprintf("[SCHED] No process to start!\n");
        for (;;)
            __asm__ volatile("hlt");
    }

    /* Pop the first entry */
    struct proc *p = run_queue[0];
    for (size_t j = 0; j + 1 < run_queue_count; j++)
        run_queue[j] = run_queue[j + 1];
    run_queue_count--;
    run_queue[run_queue_count] = NULL;

    irq_spin_unlock(&run_queue_lock);

    p->state     = PROC_RUNNING;
    current_proc = p;

    /* Set TSS RSP0 so hardware interrupts in ring 3
       switch to this process's kernel stack */
    tss_set_rsp0(p->kernel_stack_top);

    /* Load user page tables */
    load_cr3(p->cr3);

    kprintf("[SCHED] Launching PID %llu (entry=0x%llx, user_rsp=0x%llx)\n",
            p->pid, p->tf->rip, p->tf->rsp);

    /* Cache trap frame values before we reset kernel_rsp */
    uint64_t entry    = p->tf->rip;
    uint64_t user_rsp = p->tf->rsp;

    /* Reset kernel_rsp to the clean stack top for future syscalls */
    p->kernel_rsp = p->kernel_stack_top;

    /* Jump to ring 3 — never returns */
    enter_userspace(entry, user_rsp);
}

void proc_first_run(void) {
    struct proc* current_proc = my_cpu();
        
    enter_userspace(
        current_proc->tf->rip,
        current_proc->tf->rsp
    );
}