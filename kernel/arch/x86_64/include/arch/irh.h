#ifndef IRH_H
#define IRH_H

#include <arch/isr.h>

/* core exception handlers */
void handle_div_zero(struct isr_frame* f);        // 0
void handle_invalid_opcode(struct isr_frame* f);  // 6
void handle_double_fault(struct isr_frame* f);    // 8
void handle_gpf(struct isr_frame* f);             // 13
void handle_page_fault(struct isr_frame* f);      // 14

#endif