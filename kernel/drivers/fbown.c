#include <kernel/drivers/fbown.h>
#include <kernel/fbcon.h>
#include <kernel/proc/proc.h>
#include <kernel/mm/uvm.h>
#include <kernel/mm/paging.h>
#include <kernel/errno.h>
#include <kernel/console.h>

static bool owned;
static unsigned long owner_pid;
static struct vm_space *owner_mm;

static bool owner_valid(void) {
    if (!owned)
        return false;
    struct proc *p = proc_lookup(owner_pid);
    return p && !p->exited && p->mm == owner_mm && vm_has_flag(p->mm, VM_FBOWNER);
}

void fbown_check(void) {
    if (owned && !owner_valid()) {
        owned = false;
        owner_mm = NULL;
        fbcon_resume();
    }
}

bool fbown_active(void) {
    fbown_check();
    return owned;
}

int fbown_acquire(struct proc *p) {
    fbown_check();
    if (owned)
        return (p->pid == owner_pid && p->mm == owner_mm) ? 0 : -EBUSY;
    owned = true;
    owner_pid = p->pid;
    owner_mm = p->mm;
    fbcon_suspend();
    return 0;
}
