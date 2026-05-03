#include <kernel/console.h>
#include <kernel/panic.h>
#include <arch/idt.h>

void kernel_main(void) {
    // VGA text buffer at 0xB8000
    //panic("Testing Panic!!");
    idt_init();
    kprintf("IDT loaded\n");

    volatile uint64_t* p = (uint64_t*)0x40000000; // 1 GiB boundary
    *p = 1;

    while (1) {
        __asm__ volatile ("hlt");
    }
}