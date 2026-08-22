#include <kernel/proc/exec.h>
#include <kernel/proc/elf_loader.h>
#include <kernel/fs/tar.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/kheap.h>
#include <kernel/mm/uvm.h>
#include <kernel/console.h>

/* Fixed per-proc user stack VA — same for every proc, safe since each
 * has its own independent TTBR0 (see kernel/arch/aarch64/proc.c's
 * proc_init(), which this calls). One page for now; no argv/envp. */
#define USER_STACK_VA 0x500000

/* One page was enough while every payload was hand-written assembly with
 * no call depth and no locals. C code blows through that immediately —
 * a single printf frame with a format buffer can approach it — and there
 * is no guard page, so overflow silently corrupts whatever sits below
 * rather than faulting. 128KB is cheap per process and leaves room for
 * the DOOM port's call depth. */
#define USER_STACK_PAGES 32

struct proc *proc_create_from_binary(const char *binary_path) {
    struct tar_file f;
    if (!tar_open(binary_path, &f)) {
        kprintf("[EXEC] %s not found in initrd\n", binary_path);
        return NULL;
    }

    phys_addr_t pml4 = create_user_pml4();
    virt_addr_t entry;
    if (parse_and_load_binary((virt_addr_t)f.data, f.size, pml4, &entry) != 0) {
        kprintf("[EXEC] ELF load failed for %s\n", binary_path);
        return NULL;
    }

    for (size_t i = 0; i < USER_STACK_PAGES; i++) {
        phys_addr_t stack_phys = (phys_addr_t)pmm_alloc_page();
        if (!stack_phys) {
            kprintf("[EXEC] out of memory allocating stack for %s\n", binary_path);
            return NULL;
        }
        map_page(pml4, USER_STACK_VA + i * PAGE_SIZE, stack_phys,
                 PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX);
    }

    /* No MMIO mapping here any more. The UART used to be identity-mapped
     * into every process purely so kernel kprintf()s would survive a
     * TTBR0 swap — uart.c now reaches it through the kernel's own
     * high-half Device mapping (serial_remap_to_hhdm(), called by
     * init_paging()), so a user table contains only what that process
     * actually owns: its ELF segments and its stack. */

    struct proc *p = kmalloc(sizeof(struct proc));
    if (!p) {
        kprintf("[EXEC] out of memory allocating struct proc for %s\n", binary_path);
        return NULL;
    }

    /* proc_table_add() needs `p` to already exist (it stores the
     * pointer) but assigns the pid before proc_init() fills the struct
     * in — proc_init() is what actually writes p->pid, so it must run
     * with the pid proc_table_add() hands back, not before. */
    uint64_t pid = proc_table_add(p);
    proc_init(p, pid, entry, USER_STACK_VA + USER_STACK_PAGES * PAGE_SIZE, pml4);

    p->mm = vm_space_create(pml4);
    if (!p->mm) {
        kprintf("[EXEC] out of memory allocating vm_space for %s\n", binary_path);
        proc_table_remove(p);
        kfree(p);
        return NULL;
    }

    return p;
}
