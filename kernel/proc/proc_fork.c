#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <kernel/mm/uvm.h>
#include <kernel/mm/kheap.h>
#include <kernel/mm/paging.h>
#include <kernel/console.h>
#include <kernel/klibc/string.h>
#include <kernel/fs/file.h>
#include <arch/exceptions.h>
#include <arch/irq_spinlock.h>

/*
 * fork() — duplicate the calling process.
 *
 * Three distinct pieces of state have to survive into the child, and
 * each lives somewhere different at the moment this runs:
 *
 *  1. The ADDRESS SPACE, in the parent's vm_space. vm_space_clone()
 *     (kernel/mm/uvm.c) copies the private pages and shares the views.
 *
 *  2. The USER REGISTERS, in the trap frame `f` on the parent's kernel
 *     stack — put there by SAVE_CONTEXT when the `svc` trapped. Copying
 *     that frame to the child's kernel stack and pointing the child's
 *     context.sp at it means the child resumes from the same
 *     instruction with the same registers; setting x0 to 0 in the copy
 *     is the entire difference between parent and child.
 *
 *  3. The FP/SIMD registers and TPIDR_EL0, which are in neither. They
 *     are still LIVE IN THE HARDWARE: exception entry deliberately
 *     doesn't save them (the kernel is -mgeneral-regs-only, so it can't
 *     disturb them), and the parent's own struct cpu_context holds only
 *     what was saved at its last switch-out, which is stale by
 *     definition. cpu_context_save_fpsimd() reads them out of the
 *     registers into the child's context.
 *
 * Point 3 is the one that would have been silently wrong. A child that
 * inherits nothing but integer registers works perfectly for any
 * program that reloads its FP state before using it, and produces
 * garbage for one that doesn't — the same shape of bug as the FP/SIMD,
 * SP_EL0 and TPIDR_EL0 context-switch misses before it, and invisible
 * for the same reason.
 */
struct proc *proc_fork(struct proc *parent, struct aarch64_frame *f) {
    if (!parent || !parent->mm)
        return NULL;
    struct thread *caller = my_thread();
    if (!caller || caller->process != parent)
        return NULL;

    struct vm_space *mm = vm_space_clone(parent->mm);
    if (!mm) {
        kprintf("[FORK] pid %lu: could not clone address space\n",
                (unsigned long)parent->pid);
        return NULL;
    }

    struct proc *child = kmalloc(sizeof(struct proc));
    if (!child) {
        kprintf("[FORK] pid %lu: out of memory for struct proc\n",
                (unsigned long)parent->pid);
        vm_space_destroy(mm);
        return NULL;
    }

    uint64_t pid = proc_alloc_pid();
    proc_init(child, pid, caller->entry, caller->user_sp, mm->ttbr0);

    child->mm        = mm;
    child->parent    = parent;
    child->pgid      = parent->pgid;
    child->sid       = parent->sid;
    irq_spin_lock(&parent->cred_lock);
    child->ruid = parent->ruid;
    child->euid = parent->euid;
    child->suid = parent->suid;
    child->rgid = parent->rgid;
    child->egid = parent->egid;
    child->sgid = parent->sgid;
    child->supplementary_group_count = parent->supplementary_group_count;
    memcpy(child->supplementary_groups, parent->supplementary_groups,
           parent->supplementary_group_count
               * sizeof(parent->supplementary_groups[0]));
    child->file_umask = parent->file_umask;
    irq_spin_unlock(&parent->cred_lock);
    resource_process_fork(child, parent);
    child->user_argc = parent->user_argc;
    child->user_argv = parent->user_argv;
    signal_process_fork(child, parent);
    child->main_thread.signal_mask = caller->signal_mask;
    struct vfs_path parent_cwd;
    proc_cwd_snapshot(parent, &parent_cwd);
    vfs_path_release(&child->cwd);
    child->cwd = parent_cwd;
    file_table_clone(child, parent);

    /* The child's trap frame goes at the very top of its kernel stack,
     * in the same place SAVE_CONTEXT would have built one. proc_init()
     * left context.sp at kernel_stack_top; the frame occupies the 288
     * bytes below it, and context.sp moves down to match — which is
     * precisely the state RESTORE_CONTEXT expects to find. */
    struct aarch64_frame *cf =
        (struct aarch64_frame *)(child->main_thread.kernel_stack_top
                                 - sizeof(struct aarch64_frame));
    *cf = *f;
    cf->x[0] = 0;                       /* fork() returns 0 in the child */

    child->main_thread.context.sp = (uint64_t)cf;
    child->main_thread.context.lr = (uint64_t)proc_fork_trampoline;

    /* Live FP/SIMD + TPIDR_EL0, straight out of the hardware. Must come
     * after proc_init(), which zeroes the whole context. */
    cpu_context_save_fpsimd(&child->main_thread.context);

    proc_table_add(child);
    sched_policy_add(&child->main_thread);
    return child;
}

/* SAVE_CONTEXT reserves exactly this much; the child's frame is placed
 * by hand rather than by that macro, so a struct that drifted out of
 * step with it would put the frame at the wrong offset and RESTORE_
 * CONTEXT would read 288 bytes of something else. */
_Static_assert(sizeof(struct aarch64_frame) == 288,
               "exception_vectors.S SAVE_CONTEXT reserves 288 bytes");
