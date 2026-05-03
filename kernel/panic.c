#include <stdarg.h>
#include <stdint.h>

#include <kernel/console.h>
#include <kernel/panic.h>

/* halt forever */
static inline void halt(void) {
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void panic(const char* fmt, ...) {
    /* disable interrupts immediately */
    __asm__ volatile ("cli");

    /* make panic visually obvious */
    // clear_screen(0x4F);  // white on red

    kcprintf("\n=== KERNEL PANIC ===\n\n", 0x4F);

    va_list args;
    va_start(args, fmt);

    /* reuse your printf backend */
    // extern void kvprintf(const char*, char, va_list);
    kvprintf(fmt, 0x4F, args);

    va_end(args);

    kcprintf("\n\nSystem halted.\n", 0x4F);

    halt();
}