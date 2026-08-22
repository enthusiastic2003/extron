#include <kernel/proc/exec.h>
#include <kernel/proc/elf_loader.h>
#include <kernel/fs/tar.h>
#include <kernel/mm/paging.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/kheap.h>
#include <kernel/console.h>

/* Fixed per-proc user stack VA — same for every proc, safe since each
 * has its own independent TTBR0 (see kernel/arch/aarch64/proc.c's
 * proc_init(), which this calls). One page for now; no argv/envp. */
#define USER_STACK_VA 0x500000

/* uart.c's serial_putc() (kernel/drivers/uart.c) talks to the UART by
 * raw physical address, with no HHDM involved — it only ever worked
 * because boot.S's ORIGINAL TTBR0 identity table covered it. The
 * instant a real, per-proc TTBR0 is loaded (kernel/proc/sched.c's
 * schedule()), every kprintf() while that proc is current goes
 * silently into the void unless its own table maps this page too. */
#define UART_PHYS_PAGE (0xFE201000ULL)

static uint64_t next_pid = 0;

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

    phys_addr_t stack_phys = (phys_addr_t)pmm_alloc_page();
    if (!stack_phys) {
        kprintf("[EXEC] out of memory allocating stack for %s\n", binary_path);
        return NULL;
    }
    map_page(pml4, USER_STACK_VA, stack_phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX);

    uint64_t uart_page = UART_PHYS_PAGE & ~(PAGE_SIZE - 1);
    map_page(pml4, uart_page, uart_page, PAGE_PRESENT | PAGE_WRITE | PAGE_CACHE_DISABLE);

    struct proc *p = kmalloc(sizeof(struct proc));
    if (!p) {
        kprintf("[EXEC] out of memory allocating struct proc for %s\n", binary_path);
        return NULL;
    }

    proc_init(p, next_pid++, entry, USER_STACK_VA + PAGE_SIZE, pml4);
    return p;
}
