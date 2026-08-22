#include <stdint.h>
#include <kernel/console.h>
static volatile uint64_t ticks = 0;

void time_tick(void) {
    ticks++;
}

uint64_t time_now(void) {
    return ticks;
}