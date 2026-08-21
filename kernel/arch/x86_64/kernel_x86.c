#include <stdint.h>
#include <stddef.h>

#include <kernel/console.h>
#include <kernel/panic.h>
#include <kernel/arch.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/kheap.h>
#include <arch/idt.h>
#include <arch/gdt.h>
#include <arch/tss.h>
#include <arch/pic.h>
#include <arch/irq.h>
#include <arch/isr.h>
#include <arch/io.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/pit.h>
#include <kernel/fs/tar.h>
#include <kernel/klibc/string.h>
#include <kernel/proc/exec.h>
#include <kernel/proc/syscall.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/proc.h>

/*
 * x86_64's implementation of the kernel/include/kernel/arch.h contract,
 * plus the rest of the x86-only boot sequence (kernel_stage2 and its
 * dependents) that only kernel_stage1 -> arch_kernel_jump_to_stage2()
 * ever reaches. This is the x86 counterpart to
 * kernel/arch/aarch64/kernel_aarch64.c. The shared, arch-neutral part of
 * the boot sequence (init_pmm/init_paging/vmm_init/vmm_setup_stack)
 * lives directly in kernel_stage1 (kernel/kernel.c), not here.
 */

void arch_disable_interrupts(void) {
    __asm__ volatile ("cli");
}

void arch_halt_forever(void) {
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void arch_update_hw_cursor(int x, int y) {
    uint16_t pos = (uint16_t)(y * 80 + x);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void kernel_stage2(uint64_t mb2_addr);

static void init_devices(void) {
    init_kbd();
    init_timer(100);
}

void read_test_file(void) {
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

void arch_kernel_early_init(void) {
    idt_init();
    pic_remap();
}

void arch_kernel_mid_init(void) {
    gdt_reload();
}

void arch_kernel_jump_to_stage2(uint64_t mb2_addr, uint64_t new_stack_top) {
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
    while (1);
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
