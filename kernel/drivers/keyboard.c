#include <arch/io.h>
#include <kernel/console.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/klibc/string.h>
#include <kernel/time.h>
/* Static 1024-byte buffer */
static struct keyboard_buffer kbd_buf;

/* Basic scancode → ASCII table */
static const char scancode_to_ascii[] = {
    0,27,'1','2','3','4','5','6','7','8',
    '9','0','-','=','\b',
    '\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,
    '\\','z','x','c','v','b','n',
    'm',',','.','/',
    0,
    '*',
    0,
    ' ',
};

/* Initialize buffer */
void init_kbd(void) {
    memset(&kbd_buf, 0, sizeof(kbd_buf));
}

/* IRQ producer */
void kbd_buf_push(uint8_t sc) {
    uint32_t next = (kbd_buf.head + 1) % KEYBOARD_BUFFER_SIZE;

    if (next == kbd_buf.tail) {
        return; // buffer full → drop
    }

    kbd_buf.kb_buff[kbd_buf.head] = sc;
    kbd_buf.head = next;
}

/* Consumer (call from main loop) */
void process_keyboard(void) {
    while (kbd_buf.tail != kbd_buf.head) {

        uint8_t sc = kbd_buf.kb_buff[kbd_buf.tail];
        kbd_buf.tail = (kbd_buf.tail + 1) % KEYBOARD_BUFFER_SIZE;

        if (sc & 0x80)
            continue; // ignore key release

        if (sc < sizeof(scancode_to_ascii)) {
            char c = scancode_to_ascii[sc];
            if (c) {
                if(c=='\n'){
                  kprintf("\nCurrent Time is %ld seconds after boot", time_now()/100);
                }
                kprintf("%c", c);
            }
        }
    }
}