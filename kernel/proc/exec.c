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
#include <kernel/proc/trap.h>
#include <kernel/proc/sched.h>


struct proc* create_init_proc(const char* binary_path){
    struct tar_file binary_file;
    
    bool err_code = tar_open(binary_path, &binary_file);

    if(err_code == false){
        kprintf("Couldn't Load the binary file\n");
        return NULL;
    }

    phys_addr_t user_pml4 =  create_user_pml4();
    virt_addr_t entry_pt;

    int err_parse_code = parse_and_load_binary((virt_addr_t)binary_file.data, binary_file.size, user_pml4, &entry_pt);
    
    if(err_parse_code!=0){
        kprintf("Couldn't parse the binary file\n");
    }

    phys_addr_t stack_top_phys = 0;

    for (uint64_t i = 0; i < USER_STACK_SIZE; i += PAGE_SIZE) {
        phys_addr_t p = (phys_addr_t)pmm_alloc_page();
        memset(phys_to_virt_hhdm(p), 0, PAGE_SIZE);
        map_page(user_pml4, USER_STACK_TOP - USER_STACK_SIZE + i, p,
                PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX);
        if (i == USER_STACK_SIZE - PAGE_SIZE)
            stack_top_phys = p;  // save the top page
    }

    // Write initial stack via HHDM
    uint64_t *stack = (uint64_t*)((uint8_t*)phys_to_virt_hhdm(stack_top_phys) + PAGE_SIZE);

    *--stack = 0;  // auxv AT_NULL value
    *--stack = 0;  // auxv AT_NULL key
    *--stack = 0;  // envp NULL terminator
    *--stack = 0;  // argv NULL terminator
    *--stack = 0;  // argc = 0



    struct proc* p = proc_alloc(NULL);

    /* place initial trapframe at top of kernel stack */
    virt_addr_t kstack_top = p->kernel_rsp;
    kstack_top -= sizeof(struct trap_frame);
    kstack_top &= ~0xFULL;

    struct trap_frame *tf =
        (struct trap_frame*)kstack_top;

    memset(tf, 0, sizeof(*tf));

    // Calculate the user virtual RSP
    uint64_t stack_offset = (uint64_t)((uint8_t*)phys_to_virt_hhdm(stack_top_phys) + PAGE_SIZE) 
                        - (uint64_t)stack;
    tf->rsp = USER_STACK_TOP - stack_offset;

    tf->rip     = entry_pt;
    tf->cs      = USER_CS;
    tf->ss      = USER_DS;
    tf->rflags  = 0x202;

    p->state = PROC_RUNNABLE;
    p->cr3   = user_pml4;
    p->mm    = vm_space_create(user_pml4);

    /* scheduler-resume rsp */
    p->kernel_rsp = kstack_top;

    p->user_rsp = tf->rsp;

    p->tf = tf;

    return p;
}

struct proc* load_executable_from_binary(const char* binary_path, struct proc* parent){
    struct tar_file binary_file;
    
    bool err_code = tar_open(binary_path, &binary_file);

    if(err_code == false){
        kprintf("Couldn't Load the binary file\n");
        return NULL;
    }

    phys_addr_t user_pml4 =  create_user_pml4();
    virt_addr_t entry_pt;

    int err_parse_code = parse_and_load_binary((virt_addr_t)binary_file.data, binary_file.size, user_pml4, &entry_pt);
    
    if(err_parse_code!=0){
        kprintf("Couldn't parse the binary file\n");
    }

    // Allocate pages, map them R+W+USER+NX into user_pml4
    for (uint64_t i = 0; i < USER_STACK_SIZE; i += PAGE_SIZE) {
        phys_addr_t p = (phys_addr_t)pmm_alloc_page();
        memset(phys_to_virt_hhdm(p), 0, PAGE_SIZE);
        map_page(user_pml4, USER_STACK_TOP - USER_STACK_SIZE + i, p,
                PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX);
    }

    struct proc* p = proc_alloc(parent);

    /* place initial trapframe at top of kernel stack */
    virt_addr_t kstack_top = p->kernel_rsp;
    kstack_top -= sizeof(struct trap_frame);
    kstack_top &= ~0xFULL;

    struct trap_frame *tf =
        (struct trap_frame*)kstack_top;

    memset(tf, 0, sizeof(*tf));

    tf->rip     = entry_pt;
    tf->rsp     = USER_STACK_TOP - 16;
    tf->cs      = USER_CS;
    tf->ss      = USER_DS;
    tf->rflags  = 0x202;

    p->state = PROC_RUNNABLE;
    p->cr3   = user_pml4;
    p->mm    = vm_space_create(user_pml4);

    /* scheduler-resume rsp */
    p->kernel_rsp = kstack_top;

    p->user_rsp = USER_STACK_TOP - 16;

    p->tf = tf;

    p->context.rip = (uint64_t)proc_first_run;
    p->context.rsp = (uint64_t)p->kernel_rsp;

    return p;
}