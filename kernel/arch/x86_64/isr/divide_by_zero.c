#include <arch/isr.h>
#include <kernel/panic.h>

void handle_div_zero(struct isr_frame* f){

const char* err_msg = "Divide by zero error";

panic("EXCEPTION %d: %s\nRIP=%p\n",
          (int)f->vector,
          err_msg,
          (void*)f->rip);
}