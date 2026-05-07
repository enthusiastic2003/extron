#ifndef KEYBOARD_H
#define KEYBOARD_H
#pragma once

#include <stdint.h>

#define KEYBOARD_BUFFER_SIZE 1024

struct keyboard_buffer {
    uint32_t head;
    uint32_t tail;
    uint8_t  kb_buff[KEYBOARD_BUFFER_SIZE];
};

void init_kbd(void);
void kbd_buf_push(uint8_t scancode);
void process_keyboard(void);
#endif
