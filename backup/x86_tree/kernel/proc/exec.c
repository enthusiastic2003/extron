#include <kernel/proc/elf_loader.h>
#include <kernel/fs/tar.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/console.h>
#include <kernel/proc/exec.h>
#include <kernel/proc/proc.h>
#include <kernel/mm/kheap.h>
#include <arch/tss.h>
#include <stdbool.h>
#include <kernel/klibc/string.h>
#include <kernel/proc/proc.h>
#include <kernel/klibc/string.h>
#include <kernel/proc/trap.h>
#include <kernel/proc/sched.h>


int exec_load_binary(const char *binary_path, int argc, char **argv, int envc, char **envp, phys_addr_t *out_pml4, virt_addr_t *out_entry, virt_addr_t *out_stack) {
    struct tar_file binary_file;
    if (!tar_open(binary_path, &binary_file)) {
        return -1;
    }

    phys_addr_t user_pml4 = create_user_pml4();
    virt_addr_t entry_pt;

    if (parse_and_load_binary((virt_addr_t)binary_file.data, binary_file.size, user_pml4, &entry_pt) != 0) {
        return -1;
    }

    phys_addr_t stack_top_phys = 0;
    for (uint64_t i = 0; i < USER_STACK_SIZE; i += PAGE_SIZE) {
        phys_addr_t p = (phys_addr_t)pmm_alloc_page();
        memset(phys_to_virt_hhdm(p), 0, PAGE_SIZE);
        map_page(user_pml4, USER_STACK_TOP - USER_STACK_SIZE + i, p,
                PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX);
        if (i == USER_STACK_SIZE - PAGE_SIZE)
            stack_top_phys = p;
    }

    uint64_t user_rsp = USER_STACK_TOP;
    int total_string_size = 0;
    for (int i = 0; i < argc; i++) total_string_size += strlen(argv[i]) + 1;
    for (int i = 0; i < envc; i++) total_string_size += strlen(envp[i]) + 1;

    user_rsp -= total_string_size;
    
    uint8_t *stack_page_virt = (uint8_t*)phys_to_virt_hhdm(stack_top_phys);
    uint8_t *str_ptr = stack_page_virt + PAGE_SIZE - total_string_size;
    uint64_t user_str_ptr = user_rsp;
    
    uint64_t user_argv_ptrs[32];
    uint64_t user_envp_ptrs[32];
    
    for (int i = 0; i < argc; i++) {
        int len = strlen(argv[i]) + 1;
        memcpy(str_ptr, argv[i], len);
        user_argv_ptrs[i] = user_str_ptr;
        str_ptr += len;
        user_str_ptr += len;
    }
    
    for (int i = 0; i < envc; i++) {
        int len = strlen(envp[i]) + 1;
        memcpy(str_ptr, envp[i], len);
        user_envp_ptrs[i] = user_str_ptr;
        str_ptr += len;
        user_str_ptr += len;
    }
    
    user_rsp &= ~0x7ULL;
    
    uint64_t pointers_size = 8 + 8*argc + 8 + 8*envc + 8 + 16;
    user_rsp -= pointers_size;
    
    if ((user_rsp % 16) != 8) {
        user_rsp -= 8;
    }
    
    uint64_t *ptr_stack = (uint64_t *)(stack_page_virt + PAGE_SIZE - (USER_STACK_TOP - user_rsp));
    
    int p_idx = 0;
    ptr_stack[p_idx++] = argc;
    for (int i = 0; i < argc; i++) ptr_stack[p_idx++] = user_argv_ptrs[i];
    ptr_stack[p_idx++] = 0;
    
    for (int i = 0; i < envc; i++) ptr_stack[p_idx++] = user_envp_ptrs[i];
    ptr_stack[p_idx++] = 0;
    
    ptr_stack[p_idx++] = 0;
    ptr_stack[p_idx++] = 0;

    *out_pml4 = user_pml4;
    *out_entry = entry_pt;
    *out_stack = user_rsp;

    return 0;
}

struct proc* proc_create_from_binary(const char* binary_path, struct proc *parent) {
    phys_addr_t pml4;
    virt_addr_t entry;
    virt_addr_t rsp;

    if (exec_load_binary(binary_path, 0, NULL, 0, NULL, &pml4, &entry, &rsp) < 0) {
        kprintf("Couldn't load the binary file %s\n", binary_path);
        return NULL;
    }

    struct proc* p = proc_alloc(parent);

    virt_addr_t kstack_top = p->kernel_rsp;
    kstack_top -= sizeof(struct trap_frame);
    kstack_top &= ~0xFULL;

    struct trap_frame *tf = (struct trap_frame*)kstack_top;
    memset(tf, 0, sizeof(*tf));

    tf->rsp = rsp;
    tf->rip = entry;
    tf->cs = USER_CS;
    tf->ss = USER_DS;
    tf->rflags = 0x202;

    p->state = PROC_RUNNABLE;
    p->cr3 = pml4;
    p->mm = vm_space_create(pml4);
    p->kernel_rsp = kstack_top;
    p->user_rsp = rsp;
    p->tf = tf;

    p->context.rip = (uint64_t)proc_first_run;
    p->context.rsp = (uint64_t)p->kernel_rsp;

    return p;
}
