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

static struct thread *current_thread = NULL;

/* Set only while sched_idle_wait() below is parked in its wfi loop.
 * Interrupts are deliberately unmasked there, so the timer IRQ's own
 * schedule() (timer_irq_handler, kernel/drivers/timer.c) can land
 * re-entrantly on this same kernel stack — it must NOT context-switch
 * out from under the idle loop, or the idling proc's saved context
 * would point back into the loop instead of into the sleep() call it
 * actually needs to resume, and it would never return to userland. The
 * IRQ still does the part that matters (thread_wakeup_expired() making
 * things runnable); the idle loop picks the result up itself. */
static volatile int in_idle = 0;

struct thread *my_thread(void) {
    return current_thread;
}

struct proc *my_proc(void) {
    return current_thread ? current_thread->process : NULL;
}

void sched_init(void) {
    sched_policy_init();
    current_thread = NULL;
    kprintf("[SCHED] Scheduler initialized\n");
}

/*
 * Reached only via context_switch's `ret` landing on context.lr — never
 * called directly. A brand-new main thread's context is pre-populated
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
    struct thread *t = current_thread;
    struct proc *p = t->process;
    /* argc/argv land in x0/x1, where AAPCS64 puts main()'s first two
     * arguments — so entering _start is an ordinary call as far as the
     * process can tell. Before this, x0/x1 held whatever the kernel had
     * last left in them, and usr/lib/crt0.S zeroed them itself to hide
     * it; it now passes them straight through. */
    register uint64_t argc   __asm__("x0") = p->user_argc;
    register uint64_t argv   __asm__("x1") = p->user_argv;
    register uint64_t elr    __asm__("x2") = t->entry;
    register uint64_t spsr   __asm__("x3") = 0x0;   /* EL0t, all masks clear */
    register uint64_t sp_el0 __asm__("x4") = t->user_sp;
    __asm__ volatile (
        "msr elr_el1, %2\n\t"
        "msr spsr_el1, %3\n\t"
        "msr sp_el0, %4\n\t"
        "eret"
        :: "r"(argc), "r"(argv), "r"(elr), "r"(spsr), "r"(sp_el0)
        : "memory"
    );
    __builtin_unreachable();
}

/*
 * Common to schedule() and sched_start(): make `next` the current thread,
 * swapping TTBR0_EL1 if the address space actually changed (aarch64
 * counterpart to x86's proc_install()'s conditional load_cr3).
 *
 * Runs with IRQs masked, and that is not a formality.
 *
 * `current_thread = next` happens here, but SP_EL1 does not change until
 * context_switch's `mov sp, x9` several instructions later. In between,
 * the kernel is running on the OUTGOING proc's stack while my_proc()
 * already answers with the incoming one. A timer IRQ landing in that
 * window calls schedule(), which takes `old = current_thread` — i.e.
 * `next` — and context-switches away, saving the stack pointer it is
 * standing on into `next`'s saved context. `next` is thereby given the
 * previous proc's kernel stack, permanently, and the two then run
 * nested on the same stack until it is corrupted.
 *
 * That is not hypothetical. sched_start() reaches here from
 * kernel_stage2() with IRQs already enabled and SP on the BOOT stack
 * (kernel/mm/vmm.c's vmm_setup_stack()), so the very first process was
 * being handed a stack that nothing owns and that never unwinds — it
 * showed up as a Data Abort inside RESTORE_CONTEXT reading past the top
 * of 0xFFFFD000_00401000, in a proc whose own kernel stack was
 * somewhere in 0xFFFFC000_.... Intermittent, because it needed a tick
 * to land inside a handful of instructions.
 *
 * Every other caller already arrives masked (schedule() is only ever
 * reached from an exception handler, and sched_idle_wait() restores the
 * caller's mask before returning), so this closes the one path that
 * did not — while making the requirement explicit rather than inherited
 * from whoever happened to call.
 *
 * The restore only runs when context_switch RETURNS, which means `next`
 * was a previously-running proc resuming here — and the value restored
 * is the one that proc itself saved on its own stack when it switched
 * away, which is exactly right. A brand-new proc never returns here at
 * all: proc_bootstrap_trampoline()/proc_fork_trampoline() eret instead,
 * and eret takes DAIF from SPSR_EL1.
 */
static void install_and_switch(struct thread *old, struct thread *next) {
    uint64_t daif;
    __asm__ volatile ("mrs %0, daif" : "=r"(daif));
    __asm__ volatile ("msr daifset, #3" ::: "memory");

    resource_account_switch(old, next);

    next->state = THREAD_RUNNING;
    current_thread = next;

    if (!old || old->process->ttbr0 != next->process->ttbr0) {
        __asm__ volatile ("msr ttbr0_el1, %0"
                          :: "r"(next->process->ttbr0) : "memory");
        flush_tlb();
    }

    /* sched_start() has no real "old" proc — a throwaway local stands in
     * for context_switch's save-half; nothing ever reads it back, and
     * that call never returns anyway. */
    struct cpu_context scratch = {0};
    context_switch(old ? &old->context : &scratch, &next->context);

    __asm__ volatile ("msr daif, %0" :: "r"(daif) : "memory");
}

/*
 * Park until something becomes runnable, then return it. Only reached
 * when the caller has already marked itself non-runnable and the run
 * queue is empty — i.e. the whole system is genuinely idle and the only
 * thing that can change that is an interrupt (timer expiry via
 * thread_wakeup_expired(), or a keystroke via kbd_irq_handler()'s
 * wakeup()).
 *
 * Runs on the idling thread's own kernel stack rather than a dedicated
 * scheduler stack (xv6-style per-CPU scheduler context would be the
 * bigger, cleaner refactor; this is the minimal correct version).
 * IRQs must be unmasked across the wfi or the handler that ends the
 * idle can never run — every caller reaches schedule() from an
 * exception handler with DAIF masked, so the original mask is saved
 * and restored around each wait rather than assumed.
 */
static struct thread *sched_idle_wait(void) {
    uint64_t daif;
    __asm__ volatile ("mrs %0, daif" : "=r"(daif));

    in_idle = 1;
    for (;;) {
        __asm__ volatile ("msr daifclr, #3");
        __asm__ volatile ("wfi");
        __asm__ volatile ("msr daif, %0" :: "r"(daif) : "memory");

        struct thread *next = sched_policy_pick_next();
        if (next) {
            in_idle = 0;
            return next;
        }
    }
}

/*
 * schedule — pick the next runnable thread and context-switch to it.
 * Called from the timer IRQ path (kernel/drivers/timer.c) — unconditional
 * round robin, one tick = one timeslice — and from procs voluntarily
 * giving up the CPU (sys_sleep, sleep(), sys_exit).
 */
void schedule(void) {
    struct thread *old = current_thread;
    if (!old)
        return;

    /* Re-entered from an IRQ that fired during sched_idle_wait()'s wfi.
     * The handler's own bookkeeping has already run; leave the switching
     * decision to the idle loop that's still sitting below us on this
     * stack. See in_idle's comment. */
    if (in_idle)
        return;

    struct thread *next = sched_policy_pick_next();

    if (!next) {
        if (old->state == THREAD_RUNNING) {
            /* Transient gap — nothing else is queued, but `old` is still
             * perfectly resumable, so just keep running it. This is the
             * only case where returning without switching is correct. */
            return;
        }
        /* `old` has already marked itself SLEEPING/ZOMBIE, so returning
         * would resume a proc that believes it isn't running — which is
         * exactly what made SYS_SLEEP a no-op whenever the only other
         * proc was blocked in SYS_READ (heartbeat spun at full speed
         * while flagged THREAD_SLEEPING, and thread_wakeup_expired() then
         * kept re-queuing it, producing duplicate run-queue entries and
         * self-switches). Wait for a real wakeup instead. */
        /* Do not charge time spent parked in WFI to the blocked process
         * whose kernel stack happens to host the idle loop. */
        resource_account_switch(old, NULL);
        next = sched_idle_wait();
    }

    if (next == old) {
        /* We idled and `old` itself is what became runnable again (its
         * own sleep expired, or its channel was signalled). We ARE old:
         * no context switch, no address-space swap — just drop the
         * SLEEPING flag and let the caller unwind back to userland. */
        resource_account_switch(NULL, old);
        old->state = THREAD_RUNNING;
        return;
    }

    if (old->state == THREAD_RUNNING) {
        old->state = THREAD_RUNNABLE;
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
    struct thread *first = sched_policy_pick_next();
    if (!first) {
        panic("aarch64 sched_start: no process to start");
    }

    kprintf("[SCHED] Launching PID %lu TID %lu (entry=%p)\n",
            (unsigned long)first->process->pid,
            (unsigned long)first->tid, (void *)first->entry);

    install_and_switch(NULL, first);

    /* install_and_switch()'s context_switch() never returns here: the
     * very first proc has never run, so its saved context.lr points at
     * proc_bootstrap_trampoline(), which eret's and never comes back. */
    panic("aarch64 sched_start: unreachable");
}
