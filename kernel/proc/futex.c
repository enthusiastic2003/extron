#include <kernel/proc/futex.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <kernel/drivers/timer.h>
#include <kernel/sync/spinlock.h>
#include <arch/irq_spinlock.h>
#include <stddef.h>

/* One wait channel per (address space, virtual address). The fixed table keeps
 * allocation out of the sleep-lock critical section; slots with no sleepers
 * are immediately reusable. This is intentionally process-private futex
 * support. Shared-memory futexes will need a physical-page-based key. */
#define FUTEX_CHANNELS 256

struct futex_channel {
    struct proc *process;
    int *word;
    unsigned waiters;
};

static struct futex_channel channels[FUTEX_CHANNELS];
static spinlock_t futex_lock = SPINLOCK_INIT;

static struct futex_channel *find_channel(struct proc *p, int *word,
                                          bool create) {
    struct futex_channel *free_slot = NULL;
    for (size_t i = 0; i < FUTEX_CHANNELS; i++) {
        if (channels[i].waiters && channels[i].process == p
                && channels[i].word == word)
            return &channels[i];
        if (!channels[i].waiters && !free_slot)
            free_slot = &channels[i];
    }
    if (!create || !free_slot)
        return NULL;
    free_slot->process = p;
    free_slot->word = word;
    return free_slot;
}

int futex_wait(struct proc *p, int *word, int expected, uint64_t timeout_tick) {
    irq_spin_lock(&futex_lock);
    if (__atomic_load_n(word, __ATOMIC_RELAXED) != expected) {
        irq_spin_unlock(&futex_lock);
        return -2; /* EAGAIN */
    }
    struct futex_channel *channel = find_channel(p, word, true);
    if (!channel) {
        irq_spin_unlock(&futex_lock);
        return -1;
    }
    channel->waiters++;
    my_thread()->sleep_until = timeout_tick;
    sleep(channel, &futex_lock);
    channel->waiters--;
    if (!channel->waiters) {
        channel->process = NULL;
        channel->word = NULL;
    }
    irq_spin_unlock(&futex_lock);

    if (timeout_tick && timer_ticks() >= timeout_tick
            && __atomic_load_n(word, __ATOMIC_RELAXED) == expected)
        return -3; /* ETIMEDOUT */
    return 0;
}

int futex_wake(struct proc *p, int *word) {
    irq_spin_lock(&futex_lock);
    struct futex_channel *channel = find_channel(p, word, false);
    unsigned count = channel ? channel->waiters : 0;
    if (channel)
        wakeup(channel);
    irq_spin_unlock(&futex_lock);
    return (int)count;
}

void futex_cancel_thread(struct thread *t) {
    if (!t)
        return;
    irq_spin_lock(&futex_lock);
    for (size_t i = 0; i < FUTEX_CHANNELS; i++) {
        struct futex_channel *channel = &channels[i];
        if ((void *)channel != t->chan || !channel->waiters)
            continue;
        channel->waiters--;
        if (!channel->waiters) {
            channel->process = NULL;
            channel->word = NULL;
        }
        t->chan = NULL;
        break;
    }
    irq_spin_unlock(&futex_lock);
}
