#ifndef PIT_H
#define PIT_H
#define PIT_COMMAND 0x43
#define PIT_CHANNEL0 0x40
void init_timer(uint32_t freq);
#endif