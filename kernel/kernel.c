#include <kernel/console.h>
#include <kernel/panic.h>
#include <arch/idt.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/paging.h>
#include <arch/gdt.h>


void kernel_main(uint64_t mb2_addr) {

    idt_init();
    init_pmm(mb2_addr);
    init_paging(mb2_addr);
    gdt_reload();

    volatile int a = 0;
    volatile int b = 1;
    volatile int c = b/a;
    while (1) {
        __asm__ volatile ("hlt");
    }
}