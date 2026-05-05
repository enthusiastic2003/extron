#include <kernel/console.h>
#include <kernel/panic.h>
#include <arch/idt.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
#include <arch/gdt.h>
#include <stddef.h>
#include <kernel/mm/kheap.h>
#include <arch/pic.h>
#include <drivers/keyboard.h>

void kernel_stage2(uint64_t mb2_addr);



/**

 * @brief Stage 1: Initialization Phase.
 * Runs on the temporary boot stack.
 */
void kernel_stage1(uint64_t mb2_addr) {
    kprintf("--- Kernel Stage 1: Initialization ---\n");


    idt_init();
    init_pmm(mb2_addr);
    init_paging(mb2_addr);
    gdt_reload();
    vmm_init();
    pic_init();
    keyboard_init();


    // Prepare the new stack for Stage 2

    virt_addr_t new_stack_top = vmm_setup_stack();

    kprintf("Transitioning to Stage 2...\n");

    // Perform the stack switch and jump to Stage 2
    // We pass mb2_addr in RDI as the first argument to kernel_stage2
    __asm__ volatile (
        "mov %0, %%rsp\n\t"    // Set new stack pointer
        "mov %%rsp, %%rbp\n\t" // Initialize base pointer
        "push $0\n\t"          // Null return address for stack traces
        "mov %2, %%rdi\n\t"    // Pass mb2_addr to RDI (first argument)
        "jmp *%1"              // Jump to stage 2 (added * for indirect jump)
        : : "r"(new_stack_top), "r"(kernel_stage2), "r"(mb2_addr) : "memory"

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
    
    // Enable interrupts
    __asm__ volatile ("sti");
    kprintf("Interrupts enabled. Keyboard ready!\n");

    kprintf("Multiboot2 pointer is still: %p\n", (void*)mb2_addr);

    while (1) {
        __asm__ volatile ("hlt");
    }
}