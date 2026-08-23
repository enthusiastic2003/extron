#ifndef KERNEL_PROC_FUTEX_H
#define KERNEL_PROC_FUTEX_H

#include <stdint.h>

struct proc;
struct thread;

/* Private futexes, keyed by address space plus userspace VA. timeout_tick is
 * an absolute timer tick or zero for no timeout. */
int futex_wait(struct proc *p, int *word, int expected, uint64_t timeout_tick);
int futex_wake(struct proc *p, int *word);
void futex_cancel_thread(struct thread *t);

#endif
