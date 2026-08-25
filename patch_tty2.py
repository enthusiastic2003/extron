import sys

tty_h = "kernel/include/kernel/drivers/tty.h"
with open(tty_h, "r") as f:
    content = f.read()

new_decl = """
void tty_flush_input(struct tty *t);
int tty_input_ready(struct tty *t);
int tty_wait_for_input(struct tty *t, int timeout_ms);
"""
# insert before #endif
end_idx = content.find("#endif")
content = content[:end_idx] + new_decl + content[end_idx:]
with open(tty_h, "w") as f:
    f.write(content)

tty_c = "kernel/drivers/tty.c"
with open(tty_c, "r") as f:
    content = f.read()

new_impl = """
#include <kernel/drivers/timer.h>

void tty_flush_input(struct tty *t) {
    irq_spin_lock(&t->lock);
    t->in_tail = t->in_head;
    irq_spin_unlock(&t->lock);
}

int tty_input_ready(struct tty *t) {
    irq_spin_lock(&t->lock);
    int ready = t->in_head != t->in_tail;
    irq_spin_unlock(&t->lock);
    return ready;
}

int tty_wait_for_input(struct tty *t, int timeout_ms) {
    irq_spin_lock(&t->lock);
    if (t->in_head == t->in_tail && timeout_ms != 0) {
        struct thread *th = my_thread();
        if (timeout_ms > 0) {
            uint64_t ticks = ((uint64_t)timeout_ms * timer_ticks_per_second() + 999) / 1000;
            if (!ticks) ticks = 1;
            th->sleep_until = timer_ticks() + ticks;
        } else {
            th->sleep_until = 0;
        }
        sleep(&t->in_buf, &t->lock);
        th->sleep_until = 0;
    }
    int ready = t->in_head != t->in_tail;
    irq_spin_unlock(&t->lock);
    return ready;
}
"""
with open(tty_c, "a") as f:
    f.write(new_impl)

