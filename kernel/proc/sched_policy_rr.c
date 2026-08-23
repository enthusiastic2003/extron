#include <kernel/proc/sched.h>
#include <arch/irq_spinlock.h>
#include <kernel/console.h>
#include <stddef.h>

/*
 * Plain single-queue round robin — the only scheduling policy this repo
 * has ever had (kernel/proc/sched.c on x86 is the same shape: a small
 * fixed-capacity array + a lock, no priority levels). Exposed only
 * through sched_policy_init/add/pick_next (arch/sched.h) so a future
 * real MLFQ can replace just this file without touching schedule()
 * (kernel/proc/sched.c), context_switch, or the trampoline.
 */

#define RUN_QUEUE_CAPACITY 256

static struct thread *run_queue[RUN_QUEUE_CAPACITY];
static size_t       run_queue_count = 0;
static spinlock_t   run_queue_lock  = SPINLOCK_INIT;

void sched_policy_init(void) {
    irq_spin_lock(&run_queue_lock);
    for (size_t i = 0; i < RUN_QUEUE_CAPACITY; i++)
        run_queue[i] = NULL;
    run_queue_count = 0;
    irq_spin_unlock(&run_queue_lock);
}

void sched_policy_add(struct thread *t) {
    if (!t)
        return;

    t->state = THREAD_RUNNABLE;

    irq_spin_lock(&run_queue_lock);
    if (run_queue_count >= RUN_QUEUE_CAPACITY) {
        irq_spin_unlock(&run_queue_lock);
        kprintf("[SCHED] Run queue full, cannot add TID %lu\n",
                (unsigned long)t->tid);
        return;
    }
    run_queue[run_queue_count++] = t;
    irq_spin_unlock(&run_queue_lock);
}

struct thread *sched_policy_pick_next(void) {
    irq_spin_lock(&run_queue_lock);

    if (run_queue_count == 0) {
        irq_spin_unlock(&run_queue_lock);
        return NULL;
    }

    struct thread *next = run_queue[0];
    for (size_t j = 0; j + 1 < run_queue_count; j++)
        run_queue[j] = run_queue[j + 1];
    run_queue_count--;
    run_queue[run_queue_count] = NULL;

    irq_spin_unlock(&run_queue_lock);
    return next;
}
