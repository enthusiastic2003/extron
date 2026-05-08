#include <kernel/proc/sched.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/exec.h>
#include <kernel/mm/paging.h>
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
 * Run queue — simple singly-linked circular list
 * --------------------------------------------------------------- */
static struct proc *run_queue_head = NULL;
static struct proc *run_queue_tail = NULL;

/* Idle context — used when no process is running (kernel bootstrap) */
static struct cpu_context idle_context;

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
    run_queue_head = NULL;
    run_queue_tail = NULL;
    current_proc   = NULL;
    kprintf("[SCHED] Scheduler initialized\n");
}

/* ---------------------------------------------------------------
 * sched_add — append a RUNNABLE process to the run queue
 * --------------------------------------------------------------- */
void sched_add(struct proc *p) {
    if (!p)
        return;

    p->next = NULL;

    if (!run_queue_head) {
        run_queue_head = p;
        run_queue_tail = p;
    } else {
        run_queue_tail->next = p;
        run_queue_tail = p;
    }
}

/* ---------------------------------------------------------------
 * sched_remove — remove a process from the run queue
 * --------------------------------------------------------------- */
void sched_remove(struct proc *p) {
    if (!p || !run_queue_head)
        return;

    /* Head removal */
    if (run_queue_head == p) {
        run_queue_head = p->next;
        if (run_queue_tail == p)
            run_queue_tail = NULL;
        p->next = NULL;
        return;
    }

    /* Walk the list */
    struct proc *prev = run_queue_head;
    while (prev->next && prev->next != p)
        prev = prev->next;

    if (prev->next == p) {
        prev->next = p->next;
        if (run_queue_tail == p)
            run_queue_tail = prev;
        p->next = NULL;
    }
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

    /*
     * If the scheduler hasn't launched yet (sched_start not called),
     * don't attempt any scheduling.  Timer interrupts can fire before
     * the first process is running.
     */
    if (!old)
        return;

    /* Look for a different RUNNABLE process in the queue */
    struct proc *next = run_queue_head;
    while (next) {
        if (next->state == PROC_RUNNABLE)
            break;
        next = next->next;
    }

    /* No other process to run — keep running old */
    if (!next)
        return;

    /* --- We found a different process, perform the switch --- */
    sched_remove(next);

    /* Demote old: put it back in the queue if it was still running */
    if (old->state == PROC_RUNNING) {
        old->state = PROC_RUNNABLE;
        sched_add(old);
    }

    next->state  = PROC_RUNNING;
    current_proc = next;

    /* Update TSS RSP0 so ring-3 interrupts land on the right kernel stack */
    tss_set_rsp0(next->kernel_stack_top);

    /* Switch address spaces if needed */
    if (old->cr3 != next->cr3)
        load_cr3(next->cr3);

    /* Switch kernel-mode execution context */
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
    struct proc *p = run_queue_head;

    if (!p) {
        kprintf("[SCHED] No process to start!\n");
        for (;;)
            __asm__ volatile("hlt");
    }

    sched_remove(p);

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
