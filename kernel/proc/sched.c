#include <kernel/proc/sched.h>
#include <kernel/proc/proc.h>
#include <kernel/mm/paging.h>
#include <kernel/console.h>
#include <kernel/panic.h>
#include <stddef.h>

/*
 * Scheduler mechanism — deliberately policy-agnostic. All queue
 * manipulation goes through sched_policy_*() (sched_policy_rr.c today);
 * this file only ever does: pick next, swap the address space if it
 * changed, context switch. See arch/sched.h's comment on the seam.
 */

static struct proc *current_proc = NULL;

/* Set only while sched_idle_wait() below is parked in its wfi loop.
 * Interrupts are deliberately unmasked there, so the timer IRQ's own
 * schedule() (timer_irq_handler, kernel/drivers/timer.c) can land
 * re-entrantly on this same kernel stack — it must NOT context-switch
 * out from under the idle loop, or the idling proc's saved context
 * would point back into the loop instead of into the sleep() call it
 * actually needs to resume, and it would never return to userland. The
 * IRQ still does the part that matters (proc_wakeup_expired() making
 * things runnable); the idle loop picks the result up itself. */
static volatile int in_idle = 0;

struct proc *my_proc(void) {
    return current_proc;
}

void sched_init(void) {
    sched_policy_init();
    current_proc = NULL;
    kprintf("[SCHED] Scheduler initialized\n");
}

/*
 * Reached only via context_switch's `ret` landing on context.lr — never
 * called directly. A brand-new proc's context is pre-populated
 * (proc_init(), kernel/proc/proc.c) as if it had already been
 * switched out once, with lr pointing here instead of a real return
 * address, and sp at a fresh, empty kernel stack. This makes every
 * switch in schedule() — first launch or not — the exact same
 * context_switch() call, with no special case: the "resume" side of a
 * never-run proc is just this trampoline instead of an unwind back
 * through exception_dispatch(). Standard technique (xv6 calls its
 * version forkret).
 *
 * Same msr elr_el1/spsr_el1/sp_el0; eret sequence already written and
 * proven correct in kernel_aarch64.c's earlier EL0-drop test, promoted
 * into a reusable function instead of one-shot inline code. TTBR0_EL1
 * is already correctly set by schedule() before this is ever reached —
 * see below — so nothing address-space-related needs doing here.
 */
void proc_bootstrap_trampoline(void) {
    struct proc *p = current_proc;
    register uint64_t elr    __asm__("x0") = p->entry;
    register uint64_t spsr   __asm__("x1") = 0x0;   /* EL0t, all masks clear */
    register uint64_t sp_el0 __asm__("x2") = p->user_sp;
    __asm__ volatile (
        "msr elr_el1, %0\n\t"
        "msr spsr_el1, %1\n\t"
        "msr sp_el0, %2\n\t"
        "eret"
        :: "r"(elr), "r"(spsr), "r"(sp_el0)
        : "memory"
    );
    __builtin_unreachable();
}

/* Common to schedule() and sched_start(): make `next` the current proc,
 * swapping TTBR0_EL1 if the address space actually changed (aarch64
 * counterpart to x86's proc_install()'s conditional load_cr3). */
static void install_and_switch(struct proc *old, struct proc *next) {
    next->state  = PROC_RUNNING;
    current_proc = next;

    if (!old || old->ttbr0 != next->ttbr0) {
        __asm__ volatile ("msr ttbr0_el1, %0" :: "r"(next->ttbr0) : "memory");
        flush_tlb();
    }

    /* sched_start() has no real "old" proc — a throwaway local stands in
     * for context_switch's save-half; nothing ever reads it back, and
     * that call never returns anyway. */
    struct cpu_context scratch = {0};
    context_switch(old ? &old->context : &scratch, &next->context);
}

/*
 * Park until something becomes runnable, then return it. Only reached
 * when the caller has already marked itself non-runnable and the run
 * queue is empty — i.e. the whole system is genuinely idle and the only
 * thing that can change that is an interrupt (timer expiry via
 * proc_wakeup_expired(), or a keystroke via kbd_irq_handler()'s
 * wakeup()).
 *
 * Runs on the idling proc's own kernel stack rather than a dedicated
 * scheduler stack (xv6-style per-CPU scheduler context would be the
 * bigger, cleaner refactor; this is the minimal correct version).
 * IRQs must be unmasked across the wfi or the handler that ends the
 * idle can never run — every caller reaches schedule() from an
 * exception handler with DAIF masked, so the original mask is saved
 * and restored around each wait rather than assumed.
 */
static struct proc *sched_idle_wait(void) {
    uint64_t daif;
    __asm__ volatile ("mrs %0, daif" : "=r"(daif));

    in_idle = 1;
    for (;;) {
        __asm__ volatile ("msr daifclr, #3");
        __asm__ volatile ("wfi");
        __asm__ volatile ("msr daif, %0" :: "r"(daif) : "memory");

        struct proc *next = sched_policy_pick_next();
        if (next) {
            in_idle = 0;
            return next;
        }
    }
}

/*
 * schedule — pick the next runnable proc and context-switch to it.
 * Called from the timer IRQ path (kernel/drivers/timer.c) — unconditional
 * round robin, one tick = one timeslice — and from procs voluntarily
 * giving up the CPU (sys_sleep, sleep(), sys_exit).
 */
void schedule(void) {
    struct proc *old = current_proc;
    if (!old)
        return;

    /* Re-entered from an IRQ that fired during sched_idle_wait()'s wfi.
     * The handler's own bookkeeping has already run; leave the switching
     * decision to the idle loop that's still sitting below us on this
     * stack. See in_idle's comment. */
    if (in_idle)
        return;

    struct proc *next = sched_policy_pick_next();

    if (!next) {
        if (old->state == PROC_RUNNING) {
            /* Transient gap — nothing else is queued, but `old` is still
             * perfectly resumable, so just keep running it. This is the
             * only case where returning without switching is correct. */
            return;
        }
        /* `old` has already marked itself SLEEPING/ZOMBIE, so returning
         * would resume a proc that believes it isn't running — which is
         * exactly what made SYS_SLEEP a no-op whenever the only other
         * proc was blocked in SYS_READ (heartbeat spun at full speed
         * while flagged PROC_SLEEPING, and proc_wakeup_expired() then
         * kept re-queuing it, producing duplicate run-queue entries and
         * self-switches). Wait for a real wakeup instead. */
        next = sched_idle_wait();
    }

    if (next == old) {
        /* We idled and `old` itself is what became runnable again (its
         * own sleep expired, or its channel was signalled). We ARE old:
         * no context switch, no address-space swap — just drop the
         * SLEEPING flag and let the caller unwind back to userland. */
        old->state = PROC_RUNNING;
        return;
    }

    if (old->state == PROC_RUNNING) {
        old->state = PROC_RUNNABLE;
        sched_policy_add(old);
    }

    install_and_switch(old, next);
}

/*
 * sched_start — launch the very first proc. Called once from
 * kernel_aarch64_stage2(), never returns. Needs an "old" context to
 * save into even though nothing meaningful resumes there (mirrors x86's
 * proc_install(NULL, p)) — a throwaway local works fine since this call
 * never returns anyway.
 */
void sched_start(void) {
    struct proc *first = sched_policy_pick_next();
    if (!first) {
        panic("aarch64 sched_start: no process to start");
    }

    kprintf("[SCHED] Launching PID %lu (entry=%p)\n",
            (unsigned long)first->pid, (void *)first->entry);

    install_and_switch(NULL, first);

    /* install_and_switch()'s context_switch() never returns here: the
     * very first proc has never run, so its saved context.lr points at
     * proc_bootstrap_trampoline(), which eret's and never comes back. */
    panic("aarch64 sched_start: unreachable");
}
