#ifndef KERNEL_PROC_SCHED_H
#define KERNEL_PROC_SCHED_H

#include <kernel/proc/proc.h>

/* ---------------------------------------------------------------
 * Scheduling policy seam — the only surface a future real MLFQ swap
 * needs to touch. kernel/proc/sched_policy_rr.c implements these three
 * today with a plain single-queue round robin (confirmed: there is no
 * existing MLFQ anywhere in this repo's x86 history to reuse).
 * schedule() itself never touches the underlying queue directly — see
 * kernel/proc/sched.c.
 * --------------------------------------------------------------- */
void          sched_policy_init(void);
void          sched_policy_add(struct proc *p);      /* mark runnable / re-enqueue */
struct proc  *sched_policy_pick_next(void);            /* pop next to run, or NULL */

/* ---------------------------------------------------------------
 * Scheduler mechanism
 * --------------------------------------------------------------- */
void          sched_init(void);
void          schedule(void);       /* pick next + context switch; called from timer IRQ */
void          sched_start(void);    /* launch the first process; never returns */

struct proc  *my_proc(void);

/* Defined in sched.c, reached only via a saved context.lr (never called
 * directly) — see its comment for the forkret-style first-launch trick.
 * Not static: proc_init() (kernel/proc/proc.c) needs its address to
 * pre-populate a new proc's context. */
void          proc_bootstrap_trampoline(void);

/* The same idea for a forked child, in kernel/arch/aarch64/
 * exception_vectors.S: reached only via a saved context.lr, with
 * context.sp already pointing at a copy of the parent's trap frame, so
 * it restores a full register state instead of synthesizing a first
 * entry into EL0. See proc_fork() (kernel/proc/proc_fork.c). */
void          proc_fork_trampoline(void);

/* kernel/arch/aarch64/proc/switch.S */
extern void   context_switch(struct cpu_context *old, struct cpu_context *new_ctx);

/* context_switch's save half alone: captures the FP/SIMD registers,
 * FPCR/FPSR and TPIDR_EL0 that are live in the hardware right now.
 * fork() needs it because the parent's own saved context is stale — the
 * current values are in the registers, not in the struct. */
extern void   cpu_context_save_fpsimd(struct cpu_context *c);

#endif
