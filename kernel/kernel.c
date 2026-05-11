#include <kernel/console.h>
#include <kernel/panic.h>
#include <arch/idt.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
#include <arch/gdt.h>
#include <arch/tss.h>
#include <stddef.h>
#include <kernel/mm/kheap.h>
#include <arch/pic.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/pit.h>
#include <arch/irq.h>
#include <arch/isr.h>
#include <kernel/fs/tar.h>
#include <kernel/klibc/string.h>
#include <kernel/drivers/serial.h>
#include <kernel/proc/exec.h>
#include <kernel/proc/syscall.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/proc.h>

void kernel_stage2(uint64_t mb2_addr);

void init_devices(){
    init_kbd();
    init_timer(100);
}

void read_test_file(){
    tar_list();
    struct tar_file file;
    if (tar_open("./test.txt", &file)) {
        kprintf("Opened file: %s\n", file.name);
        kprintf("File size: %zu bytes\n", file.size);
        // Assuming file.data points to the start of the content and is null-terminated
        // We cast to char* to print it. This relies on the tarball containing a C-style string.
        char *buf = (char*)kmalloc(file.size + 1);
        if (buf) {
            memcpy(buf, file.data, file.size);
            buf[file.size] = '\0';
            kprintf("Content:\n%s\n", buf);
            kfree(buf);
        }
    } else {
        kprintf("Failed to open test.txt\n");
    }
}

/**

 * @brief Stage 1: Initialization Phase.
 * Runs on the temporary boot stack.
 */
void kernel_stage1(uint64_t mb2_addr) {
    init_serial();

    kprintf("--- Kernel Stage 1: Initialization ---\n");


    idt_init();
    pic_remap();
    init_pmm(mb2_addr);
    gdt_reload();
    init_paging(mb2_addr);
    pmm_print_stats();
    vmm_init();

    // Prepare the new stack for Stage 2

    virt_addr_t new_stack_top = vmm_setup_stack();

    kprintf("Transitioning to Stage 2...\n");

    // Perform the stack switch and jump to Stage 2
    // We pass mb2_addr in RDI as the first argument to kernel_stage2
    __asm__ volatile (
        "mov %0, %%rsp\n\t"
        "mov %%rsp, %%rbp\n\t"
        "push $0\n\t"
        "jmp *%1"
        :
        : "r"(new_stack_top), "r"(kernel_stage2), "D"(mb2_addr)
        : "memory"
    );

    // We should never reach here
    while(1);
}

/**
 * @brief Stage 2: The "Real" Kernel entry point.
 * This function runs on the permanent 4MB virtual stack.
 */
void kernel_stage2(uint64_t mb2_addr) {
    kprintf("--- Kernel Stage 2: Execution Phase ---\n");
    kprintf("Successfully running on high-half virtual stack.\n");

    tss_init(KERNEL_STACK_RANGE_START + KERNEL_STACK_GUARD + KERNEL_STACK_SIZE);
    kprintf("TSS: loaded (RSP0 = %p)\n",
            (void*)(KERNEL_STACK_RANGE_START + KERNEL_STACK_GUARD + KERNEL_STACK_SIZE));

    syscall_init();

    init_devices();
    tar_init(mb2_addr);
    init_irq(); 
    enable_interrupt();
    
    /* --- Scheduler bootstrap --- */
    proc_table_init();
    sched_init();

    struct proc *init  = create_init_proc("./test2");
    struct proc *init2 = load_executable_from_binary("./test", init);

    if (init && init2) {
        sched_add(init);
        sched_add(init2);
    } else {
        kprintf("Failed to create init processes!\n");
    }

    sched_start();  /* never returns */

    /* Should never reach here */
    while (1) {
        __asm__ volatile ("hlt");
    }
}