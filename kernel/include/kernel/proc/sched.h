#ifndef SCHED_H
#define SCHED_H

#include <kernel/proc/proc.h>

/* ---------------------------------------------------------------
 * Scheduler API
 * --------------------------------------------------------------- */

void           sched_init(void);

void           sched_add(struct proc *p);       /* add to run queue */
void           sched_remove(struct proc *p);    /* remove from run queue */

void           schedule(void);                  /* pick next + context switch */
void           sched_start(void);               /* launch first process (never returns) */

/* Returns the currently running process.
 * All C code should use this instead of touching any global. */
struct proc   *my_cpu(void);

/* Assembly routine in switch.asm */
extern void    context_switch(struct cpu_context *old, struct cpu_context *new_ctx);

#endif /* SCHED_H */
