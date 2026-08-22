#include <kernel/drivers/serial.h>
#include <arch/io.h>

#define SERIAL_PORT 0x3F8
static int serial_initialized = 0;

void init_serial(void) {
    outb(SERIAL_PORT + 1, 0x00);    // Disable all interrupts
    outb(SERIAL_PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(SERIAL_PORT + 0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
    outb(SERIAL_PORT + 1, 0x00);    //                  (hi byte)
    outb(SERIAL_PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(SERIAL_PORT + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
    outb(SERIAL_PORT + 4, 0x03);    // IRQs disabled, RTS/DSR set
    outb(SERIAL_PORT + 4, 0x1E);    // Set in loopback mode, test the serial chip
    outb(SERIAL_PORT + 0, 0xAE);    // Test serial chip (send byte 0xAE and check if serial returns same byte)
    
    // Check if serial is faulty (i.e: not same byte as sent)
    if(inb(SERIAL_PORT + 0) != 0xAE) {
        return;
    }
    
    // If serial is not faulty set it in normal operation mode
    // (not-loopback with IRQs disabled and OUT#1/OUT#2 disabled)
    outb(SERIAL_PORT + 4, 0x03);
    serial_initialized = 1;
}

void serial_putc(char c) {
    if (!serial_initialized) return;
    // Wait for transmit empty
    while ((inb(SERIAL_PORT + 5) & 0x20) == 0);
    outb(SERIAL_PORT, c);
}
