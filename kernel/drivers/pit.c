#include <stdint.h>
#include <arch/io.h>
#include <kernel/drivers/pit.h>

void init_timer(uint32_t freq) {
    uint32_t divisor = 1193182 / freq;

    outb(PIT_COMMAND, 0x36); // channel 0, lobyte/hibyte, mode 3

    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);
}