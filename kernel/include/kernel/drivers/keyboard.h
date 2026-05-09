#ifndef KEYBOARD_H
#define KEYBOARD_H
#pragma once

#include <stdint.h>
#include <kernel/sync/spinlock.h>

#define KEYBOARD_BUFFER_SIZE 1024

struct keyboard_buffer {
    uint8_t kb_buff[KEYBOARD_BUFFER_SIZE];
    uint32_t head;
    uint32_t tail;
    spinlock_t lock; // Protects head, tail, and kb_buff
};

static struct keyboard_buffer kbd_buf;

void init_kbd(void);
void kbd_buf_push(uint8_t scancode);
// void process_keyboard(void);
uint64_t kbd_read(char *buf, uint64_t count); // ← new
#endif
