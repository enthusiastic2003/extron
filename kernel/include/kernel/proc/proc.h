#ifndef PROC_H
#define PROC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/proc/trap.h>
#include <kernel/sync/spinlock.h>

/* Segment selectors — must match GDT layout */
#define USER_CS 0x23
#define USER_DS 0x1B

#define KERNEL_CS 0x08
#define KERNEL_DS 0x10

/* Size of the per-process kernel stack (used for interrupts + syscalls) */
#define PROC_KERNEL_STACK_PAGES 8           /* 32 KB */

/* -------------------------------------------------------------
 * Scheduler switch context
 *
 * Only callee-saved registers + continuation state.
 * Saved/restored by switch.asm (context_switch).
 *
 * Field order must match the offsets used in switch.asm:
 *   r15  = 0x00    r14  = 0x08    r13  = 0x10
 *   r12  = 0x18    rbx  = 0x20    rbp  = 0x28
 *   rip  = 0x30    rsp  = 0x38
 * ------------------------------------------------------------- */
struct cpu_context {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;

    uint64_t rbx;
    uint64_t rbp;

    uint64_t rip;
    uint64_t rsp;
};

/* -------------------------------------------------------------
 * Process states
 * ------------------------------------------------------------- */
enum proc_state {
    PROC_UNUSED = 0,
    PROC_RUNNABLE,
    PROC_RUNNING,
    PROC_SLEEPING,
    PROC_ZOMBIE
};

/* -------------------------------------------------------------
 * Process control block
 *
 * IMPORTANT: kernel_rsp MUST be at offset 0 and user_rsp at
 * offset 8 — syscall_entry.asm hardcodes these offsets for
 * the fast-path stack switch.
 * ------------------------------------------------------------- */
struct proc {
    /* ---- offsets 0x00, 0x08: must match syscall_entry.asm ---- */
    uint64_t        kernel_rsp;     /* 0x00 */
    uint64_t        user_rsp;       /* 0x08 */

    virt_addr_t     kernel_stack_base;
    virt_addr_t     kernel_stack_top;

    uint64_t        pid;
    enum proc_state state;

    void            *chan;          /* <--- ADD THIS: The wait channel */

    phys_addr_t     cr3;

    struct cpu_context context;

    struct trap_frame *tf;

    struct proc       *parent;
    struct proc       *next;        /* run-queue link */
};

static const char *proc_state_str(enum proc_state s) {
    switch (s) {
        case PROC_UNUSED:    return "UNUSED";
        case PROC_RUNNABLE:  return "RUNNABLE";
        case PROC_RUNNING:   return "RUNNING";
        case PROC_SLEEPING:  return "SLEEPING";
        case PROC_ZOMBIE:    return "ZOMBIE";
        default:             return "UNKNOWN";
    }
}

/* -------------------------------------------------------------
 * Process management
 * ------------------------------------------------------------- */
struct proc* proc_alloc(struct proc *parent);

void proc_free(struct proc *p);

void proc_destroy(struct proc *p);

/* -------------------------------------------------------------
 * State helpers
 * ------------------------------------------------------------- */
void proc_set_runnable(struct proc *p);

void proc_set_running(struct proc *p);

void proc_set_sleeping(struct proc *p);

void proc_set_zombie(struct proc *p);

void wakeup(void *chan);
void sleep(void *chan, spinlock_t *lk);
void proc_table_init(void);
void proc_dump_table(void);

#endif /* PROC_H */
