#ifndef KEYBOARD_H
#define KEYBOARD_H
#include <arch/isr.h>

#define KEYBOARD_BUFFER_SIZE 1024

struct keyboard_buffer
{
  uint64_t cursor_position;
  char kb_buff[KEYBOARD_BUFFER_SIZE];
};

void keyboard_handler(struct isr_frame* f);
void init_kbd();
void process_keyboard(void);
#endif
