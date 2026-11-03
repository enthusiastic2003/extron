#include <kernel/drivers/serial.h>
#include <arch/irq.h>
#include "io.h"

/* 16550 UART on COM1 — the x86 counterpart of drivers/uart.c's PL011. */
#define COM1 0x3F8

void init_serial(void) {
    outb(COM1 + 1, 0x00);   /* no interrupts yet */
    outb(COM1 + 3, 0x80);   /* DLAB on */
    outb(COM1 + 0, 0x01);   /* 115200 baud (divisor 1) */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1, DLAB off */
    outb(COM1 + 2, 0xC7);   /* enable + clear FIFOs, 14-byte threshold */
    outb(COM1 + 4, 0x0B);   /* DTR | RTS | OUT2 (OUT2 gates the IRQ line) */
}

void serial_remap_to_hhdm(void) { /* port I/O: nothing to remap */ }

void serial_putc(char c) {
    while (!(inb(COM1 + 5) & 0x20)) { }
    outb(COM1, (uint8_t)c);
}

int serial_try_getc(void) {
    if (!(inb(COM1 + 5) & 0x01))
        return -1;
    return inb(COM1);
}

char serial_getc(void) {
    int c;
    while ((c = serial_try_getc()) < 0) { __asm__ volatile ("pause"); }
    return (char)c;
}

void serial_enable_rx_irq(void) {
    outb(COM1 + 1, 0x01);   /* IER: received-data-available */
}
