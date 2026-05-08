#include <kernel/proc/proc.h>

#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/kheap.h>

#include <kernel/proc/elf_loader.h>

#include <kernel/fs/tar.h>

#include <kernel/console.h>
#include <kernel/klibc/string.h>
#include <kernel/proc/exec.h>

#include <stdbool.h>

static uint64_t next_pid = 0;

/* -------------------------------------------------------------
 * Process allocation
 * ------------------------------------------------------------- */

struct proc* proc_alloc(struct proc* parent) {

    struct proc* p = kmalloc(sizeof(struct proc));

    if (!p)
        return NULL;

    memset(p, 0, sizeof(*p));

    p->pid   = next_pid++;
    p->state = PROC_UNUSED;

    p->kernel_stack_base =
        vmm_alloc_pages(PROC_KERNEL_STACK_PAGES);

    p->kernel_rsp =
        p->kernel_stack_base +
        PROC_KERNEL_STACK_PAGES * PAGE_SIZE;

    p->kernel_stack_top = p->kernel_rsp;

    if (parent) {
        p->parent = parent;
    }
    else {
        p->parent = NULL;
    }

    return p;
}

/* -------------------------------------------------------------
 * Free low-level proc resources
 * ------------------------------------------------------------- */

void proc_free(struct proc *p) {

    if (!p)
        return;

    if (p->kernel_stack_base) {

        vmm_free_pages(
            p->kernel_stack_base,
            PROC_KERNEL_STACK_PAGES
        );
    }

    kfree(p);
}

/* -------------------------------------------------------------
 * Destroy process
 * ------------------------------------------------------------- */

void proc_destroy(struct proc *p) {

    if (!p)
        return;

    /*
     * TODO:
     * - destroy userspace address space
     * - free page tables
     * - close files
     * - release cwd
     * - notify parent/waiters
     */

    proc_set_zombie(p);

    proc_free(p);
}


/* -------------------------------------------------------------
 * State helpers
 * ------------------------------------------------------------- */

void proc_set_runnable(struct proc *p) {

    if (!p)
        return;

    p->state = PROC_RUNNABLE;
}

void proc_set_running(struct proc *p) {

    if (!p)
        return;

    p->state = PROC_RUNNING;
}

void proc_set_sleeping(struct proc *p) {

    if (!p)
        return;

    p->state = PROC_SLEEPING;
}

void proc_set_zombie(struct proc *p) {

    if (!p)
        return;

    p->state = PROC_ZOMBIE;
}