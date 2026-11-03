#include <kernel/proc/futex.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <kernel/drivers/timer.h>
#include <kernel/sync/spinlock.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/uvm.h>
#include <arch/irq_spinlock.h>
#include <stddef.h>
#include <stdbool.h>

/* One wait channel per futex word. A word in private memory is identified by
 * (address space, virtual address). A word inside a shared mapping (MAP_SHARED
 * anonymous or file memory, POSIX semaphores in shared memory, ...) is identified
 * by its physical address instead, so processes that map the same page wait on
 * and wake the same channel. The fixed table keeps allocation out of the
 * sleep-lock critical section; slots with no sleepers are immediately reusable. */
#define FUTEX_CHANNELS 256

struct futex_channel {
    struct proc *process;       /* private words only */
    int *word;                  /* private words only */
    uint64_t phys;              /* shared words: physical address of the word */
    bool shared;
    unsigned waiters;
};

static struct futex_channel channels[FUTEX_CHANNELS];
static spinlock_t futex_lock = SPINLOCK_INIT;

/* Is `word` in a shared mapping? If so, report the word's physical address. */
static bool futex_shared_key(struct proc *p, int *word, uint64_t *phys) {
    uint64_t va = (uint64_t)word;
    if (!p->mm)
        return false;
    for (struct vma *v = p->mm->vmas; v; v = v->next) {
        if (va >= v->base && va < v->base + v->size) {
            if (!v->backing)
                return false;
            uint64_t page = virt_to_phys(p->ttbr0, va & ~(uint64_t)(PAGE_SIZE - 1));
            if (!page)
                return false;
            *phys = page + (va & (PAGE_SIZE - 1));
            return true;
        }
    }
    return false;
}

static struct futex_channel *find_channel(struct proc *p, int *word,
                                          bool create) {
    uint64_t phys = 0;
    bool shared = futex_shared_key(p, word, &phys);
    struct futex_channel *free_slot = NULL;
    for (size_t i = 0; i < FUTEX_CHANNELS; i++) {
        if (channels[i].waiters) {
            if (shared ? (channels[i].shared && channels[i].phys == phys)
                       : (!channels[i].shared && channels[i].process == p
                          && channels[i].word == word))
                return &channels[i];
        } else if (!free_slot) {
            free_slot = &channels[i];
        }
    }
    if (!create || !free_slot)
        return NULL;
    free_slot->shared = shared;
    free_slot->phys = phys;
    free_slot->process = shared ? NULL : p;
    free_slot->word = shared ? NULL : word;
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
        channel->shared = false;
    }
    irq_spin_unlock(&futex_lock);

    if (signal_pending_unblocked(my_thread()))
        return -4; /* EINTR */

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
            channel->shared = false;
        }
        t->chan = NULL;
        break;
    }
    irq_spin_unlock(&futex_lock);
}
