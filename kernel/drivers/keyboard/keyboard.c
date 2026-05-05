#include <drivers/keyboard.h>
#include <arch/io.h>
#include <arch/irh.h>
#include <kernel/console.h>

#define KEYBOARD_PORT 0x60

static const char scancode_to_ascii[] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0
};

static void keyboard_callback(struct isr_frame* f) {
    (void)f;
    uint8_t scancode = inb(KEYBOARD_PORT);

    // If the top bit is set, it's a "key released" event
    if (scancode & 0x80) {
        return;
    }

    if (scancode < sizeof(scancode_to_ascii)) {
        char c = scancode_to_ascii[scancode];
        if (c != 0) {
            kprintf("%c", c); // Print to console for now
        }
    }
}

void keyboard_init() {
    isr_register_handler(33, keyboard_callback);
}
