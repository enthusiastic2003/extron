#include <arch/io.h>
#include <arch/irq.h>
#include <kernel/console.h>
#include <stdint.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/mm/kheap.h>
#include <kernel/klibc/string.h>

const char scancode_to_ascii[] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', /* 9 */
  '9', '0', '-', '=', '\b', /* Backspace */
  '\t',			/* Tab */
  'q', 'w', 'e', 'r',	/* 19 */
  't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',	/* Enter key */
    0,			/* 29   - Control */
  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',	/* 39 */
 '\'', '`',   0,		/* Left shift */
 '\\', 'z', 'x', 'c', 'v', 'b', 'n',			/* 49 */
  'm', ',', '.', '/',   0,				/* Right shift */
  '*',
    0,	/* Alt */
  ' ',	/* Space bar */
    0,	/* Caps lock */
    0,	/* 59 - F1 key ... > */
    0,   0,   0,   0,   0,   0,   0,   0,
    0,	/* < ... F10 */
    0,	/* 69 - Num lock*/
    0,	/* Scroll Lock */
    0,	/* Home key */
    0,	/* Up Arrow */
    0,	/* Page Up */
  '-',
    0,	/* Left Arrow */
    0,
    0,	/* Right Arrow */
  '+',
    0,	/* 79 - End key*/
    0,	/* Down Arrow */
    0,	/* Page Down */
    0,	/* Insert Key */
    0,	/* Delete Key */
    0,   0,   0,
    0,	/* F11 Key */
    0,	/* F12 Key */
    0, /* All other keys are undefined */
};


static struct keyboard_buffer* kbd_buf;


void init_kbd(){
  void*  kbd_buff_mem = kmalloc(sizeof(struct keyboard_buffer));
  kprintf("Keyboard Buffer at: %p\n", kbd_buff_mem);
  memset(kbd_buff_mem, 0, sizeof(struct keyboard_buffer));
  kbd_buf = (struct keyboard_buffer*)kbd_buff_mem;

  kbd_buf->cursor_position=0;

}


void kbd_buf_push(uint8_t sc){
  
  if(kbd_buf->cursor_position>=KEYBOARD_BUFFER_SIZE){
    return;
  }

  kbd_buf->kb_buff[kbd_buf->cursor_position] = sc;
  kbd_buf->cursor_position++;
}



void keyboard_handler(struct isr_frame* f) {
    (void)f;
    uint8_t sc = inb(0x60);
    kbd_buf_push(sc);
}

void process_keyboard(void) {
  int tail = 0;
  int head = kbd_buf->cursor_position;
    while (tail != head) {
        uint8_t sc = kbd_buf->kb_buff[tail];
        tail = (tail + 1) % KEYBOARD_BUFFER_SIZE;

        if (!(sc & 0x80)) {
            if (sc < sizeof(scancode_to_ascii)) {
                char c = scancode_to_ascii[sc];
                if (c) {
                    kprintf("%c", c);
                }
            }
        }
    }

    // We are done consuming, clear buffer.
    memset((void*)kbd_buf, 0, sizeof(struct keyboard_buffer));
}