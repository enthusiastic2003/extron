#include <arch/isr.h>
#include <kernel/time.h>
#include <kernel/proc/sched.h>
#include <kernel/console.h>

static int timer_debug_count = 0;

void timer_handler(struct isr_frame* f) {
    time_tick();

    
        // kprintf("[TIMER] vec=%llu cs=0x%llx ss=0x%llx rip=0x%llx rsp=0x%llx\n",
        //         f->vector, f->cs, f->ss, f->rip, f->rsp);
    timer_debug_count++;
    
    schedule();
}