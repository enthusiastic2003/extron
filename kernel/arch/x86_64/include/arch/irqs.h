#ifndef IRQS_H
#define IRQS_H

#include<arch/isr.h>
#include<stddef.h>

void keyboard_handler(struct isr_frame*);
void timer_handler(struct isr_frame* f);

#endif