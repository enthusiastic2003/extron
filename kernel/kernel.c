#include <kernel/console.h>
#include <kernel/panic.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <stddef.h>
#include <kernel/drivers/serial.h>

#ifdef __x86_64__
#include <arch/idt.h>
#include <kernel/mm/paging.h>
#include <arch/gdt.h>
#include <arch/tss.h>
#include <kernel/mm/kheap.h>
#include <arch/pic.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/pit.h>
#include <arch/irq.h>
#include <arch/isr.h>
#include <kernel/fs/tar.h>
#include <kernel/klibc/string.h>
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
#endif /* __x86_64__ */

/**
 * @brief Stage 1: Initialization Phase.
 * Runs on the temporary boot stack.
 *
 * Shared entry point for both x86_64 and aarch64. mb2_addr is either the
 * real GRUB multiboot2 pointer (x86) or a synthesized multiboot2 MMAP tag
 * built from FDT-derived memory regions (aarch64 — see
 * kernel/arch/aarch64/mb2_shim.c and kernel_aarch64_main, which calls this
 * function directly). PMM bring-up is identical on both; everything past
 * it (paging, GDT/IDT, scheduler) is still x86-only until Milestone 3+.
 */
void kernel_stage1(uint64_t mb2_addr) {
    init_serial();

    kprintf("--- Kernel Stage 1: Initialization ---\n");

#ifdef __x86_64__
    idt_init();
    pic_remap();
#endif

    init_pmm(mb2_addr);

#ifdef __x86_64__
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
#else
    pmm_print_stats();
    kprintf("aarch64: Stage 1 complete. No Stage 2 yet (paging/GDT/IDT/scheduler not implemented).\n");
    for (;;) {
        __asm__ volatile ("wfe");
    }
#endif
}

#ifdef __x86_64__
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

    struct proc *init  = proc_create_from_binary("./test", NULL);
    // struct proc *sleep_proc = proc_create_from_binary("./sleep_test", init);

    if (init) {
        sched_add(init);
    } else {
        kprintf("Failed to create init process!\n");
    }

    // if (sleep_proc) {
    //     sched_add(sleep_proc);
    // } else {
    //     kprintf("Failed to create sleep_test process!\n");
    // }

    sched_start();  /* never returns */

    /* Should never reach here */
    while (1) {
        __asm__ volatile ("hlt");
    }
}
#endif /* __x86_64__ */