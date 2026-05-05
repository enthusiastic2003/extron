#include <kernel/panic.h>
#include <arch/isr.h>
#include <arch/irh.h>
#include <arch/pic.h>

static const char* exception_names[] = {
    "Divide by Zero",              // 0
    "Debug",                       // 1
    "Non-maskable Interrupt",      // 2
    "Breakpoint",                  // 3
    "Overflow",                    // 4
    "Bound Range Exceeded",        // 5
    "Invalid Opcode",              // 6
    "Device Not Available",        // 7
    "Double Fault",                // 8
    "Coprocessor Segment Overrun", // 9
    "Invalid TSS",                 // 10
    "Segment Not Present",         // 11
    "Stack Fault",                 // 12
    "General Protection Fault",    // 13
    "Page Fault",                  // 14
    "Reserved",                    // 15
    "x87 Floating Point",          // 16
    "Alignment Check",             // 17
    "Machine Check",               // 18
    "SIMD Floating Point",         // 19
};

typedef void (*isr_handler_fn)(struct isr_frame*);

static void timer_callback_dummy(struct isr_frame* f) {
    (void)f;
    // Just acknowledge the interrupt
}

static isr_handler_fn handlers[256] = {
    [0]  = handle_div_zero,
    [6]  = handle_invalid_opcode,
    [8]  = handle_double_fault,
    [13] = handle_gpf,
    [14] = handle_page_fault,
    [32] = timer_callback_dummy, 
};



void isr_register_handler(uint8_t vector, void (*handler)(struct isr_frame*)) {
    handlers[vector] = handler;
}

void isr_handler(struct isr_frame* f) {
    /* Dispatch if handler exists */
    if (f->vector < 256 && handlers[f->vector]) {
        handlers[f->vector](f);
        
        // If it was a hardware interrupt (remapped PIC starts at 32), send EOI
        if (f->vector >= 32 && f->vector <= 47) {
            pic_send_eoi(f->vector - 32);
        }
        return;
    }

    /* Fallback */
    const char* name = "Exception Not handled (>19)";

    if (f->vector < 20) {
        name = exception_names[f->vector];
    }

    panic("EXCEPTION %d: %s\nRIP=%p\n",
          (int)f->vector,
          name,
          (void*)f->rip);
}