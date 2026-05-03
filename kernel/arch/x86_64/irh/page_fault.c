#include <stdint.h>
#include <kernel/panic.h>
#include <arch/isr.h>

/* Read CR2 (faulting virtual address) */
static inline uint64_t read_cr2(void) {
    uint64_t val;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
}

/* Page Fault (vector 14) */
void handle_page_fault(struct isr_frame* f) {
    uint64_t addr = read_cr2();        // faulting address
    uint64_t err  = f->error_code;     // CPU-provided error code

    /* Decode error bits */
    const char* present  = (err & 1)  ? "protection violation" : "non-present page";
    const char* access   = (err & 2)  ? "write" : "read";
    const char* mode     = (err & 4)  ? "user" : "kernel";
    const char* reserved = (err & 8)  ? "reserved-bit violation" : "";
    const char* fetch    = (err & 16) ? "instruction fetch" : "";

    panic("PAGE FAULT\n"
          "addr=%p\n"
          "type=%s\n"
          "access=%s\n"
          "mode=%s\n"
          "%s %s\n"
          "RIP=%p\n",
          (void*)addr,
          present,
          access,
          mode,
          reserved,
          fetch,
          (void*)f->rip);
}