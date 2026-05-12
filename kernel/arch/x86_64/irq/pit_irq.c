#include <arch/isr.h>
#include <kernel/time.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/proc.h>
#include <kernel/console.h>

static int timer_debug_count = 0;

void timer_handler(struct isr_frame* f) {
    time_tick();
    proc_wakeup_expired(time_now());

    timer_debug_count++;
    
    // Dump process table every 5 seconds (100 Hz * 5)
    if (timer_debug_count % 500 == 0) {
        proc_dump_table();
    }
    
    if ((f->cs & 3) == 3) {
        schedule();

    }



}