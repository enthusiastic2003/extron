#include <arch/proc.h>
#include <arch/sched.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/kheap.h>
#include <kernel/panic.h>

void proc_init(struct proc *p, uint64_t pid, virt_addr_t entry,
                virt_addr_t user_sp, phys_addr_t ttbr0) {
    p->pid   = pid;
    p->state = PROC_UNUSED;
    p->ttbr0 = ttbr0;
    p->entry = entry;
    p->user_sp = user_sp;
    p->next  = NULL;

    /* Through kmalloc (kernel/mm/kheap.c), not vmm_alloc_pages() directly
     * — kheap.c is itself just a byte-granularity layer on top of the
     * same vmm_alloc_pages()/vmm_free_pages() bitmap allocator, so this
     * is still page-backed underneath, just going through the proper
     * kernel allocator instead of reaching past it. */
    p->kernel_stack_base = (virt_addr_t)kmalloc(PROC_KERNEL_STACK_PAGES * PAGE_SIZE);
    if (!p->kernel_stack_base)
        panic("aarch64 proc_init: kernel stack allocation failed");
    p->kernel_stack_top = p->kernel_stack_base + PROC_KERNEL_STACK_PAGES * PAGE_SIZE;

    /* forkret-style bootstrap: pre-populate the saved context as if this
     * proc had already been switched out once, with lr pointing at the
     * trampoline instead of a real return address, and sp at a fresh,
     * empty stack. schedule()'s first switch to this proc is then the
     * exact same context_switch() call as every other switch — see
     * proc_bootstrap_trampoline()'s comment in sched.c. */
    p->context = (struct cpu_context){0};
    p->context.lr = (uint64_t)proc_bootstrap_trampoline;
    p->context.sp = p->kernel_stack_top;
}
