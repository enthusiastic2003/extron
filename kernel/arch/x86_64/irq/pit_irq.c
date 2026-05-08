#include <arch/isr.h>
#include <kernel/time.h>
#include <kernel/proc/sched.h>

void timer_handler(struct isr_frame* f) {
    (void)f;
    time_tick();
    schedule();
}