#include<arch/isr.h>
#include<stddef.h>
#include<kernel/drivers/keyboard.h>
#include<arch/io.h>

void keyboard_handler(struct isr_frame* f) {
    (void)f;
    uint8_t sc = inb(0x60);
    kbd_buf_push(sc);
    process_keyboard();
}