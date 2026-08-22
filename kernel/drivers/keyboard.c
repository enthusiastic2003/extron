#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/serial.h>

void init_kbd(void) {
    /* Nothing to do — init_serial() (kernel/drivers/uart.c) already
     * enables both TX and RX on UART0 before this is ever called. */
}

char kbd_getc(void) {
    return serial_getc();
}
