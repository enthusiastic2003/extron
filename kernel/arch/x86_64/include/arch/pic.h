#ifndef ARCH_X86_64_PIC_H
#define ARCH_X86_64_PIC_H

#include <stdint.h>

void pic_init();
void pic_send_eoi(uint8_t irq);

#endif
