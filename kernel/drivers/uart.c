#include <kernel/drivers/serial.h>

// BCM2711 (RPi4) peripheral base + PL011 UART0 / GPIO offsets.
// Matches both real hardware and QEMU's raspi4b machine.
#define PERIPHERAL_BASE 0xFE000000UL
#define GPIO_BASE       (PERIPHERAL_BASE + 0x200000)
#define UART0_BASE      (PERIPHERAL_BASE + 0x201000)

#define GPFSEL1         (GPIO_BASE + 0x04)
#define GPIO_PUP_PDN0   (GPIO_BASE + 0xE4)

#define UART0_DR        (UART0_BASE + 0x00)
#define UART0_FR        (UART0_BASE + 0x18)
#define UART0_IBRD      (UART0_BASE + 0x24)
#define UART0_FBRD      (UART0_BASE + 0x28)
#define UART0_LCRH      (UART0_BASE + 0x2C)
#define UART0_CR        (UART0_BASE + 0x30)
#define UART0_ICR       (UART0_BASE + 0x44)

static inline void mmio_write(unsigned long addr, unsigned int val) {
    *(volatile unsigned int *)addr = val;
}

static inline unsigned int mmio_read(unsigned long addr) {
    return *(volatile unsigned int *)addr;
}

static inline void delay(volatile int count) {
    while (count--) {
        __asm__ volatile("nop");
    }
}

void init_serial(void) {
    // Disable UART0 while configuring it.
    mmio_write(UART0_CR, 0);

    // GPIO14/15 -> ALT0 (TXD0/RXD0).
    unsigned int sel = mmio_read(GPFSEL1);
    sel &= ~((7u << 12) | (7u << 15));
    sel |= (4u << 12) | (4u << 15); // ALT0 = 0b100
    mmio_write(GPFSEL1, sel);

    // No pull up/down on GPIO14/15 (BCM2711 pull scheme: 2 bits/pin, 00 = none).
    unsigned int pup = mmio_read(GPIO_PUP_PDN0);
    pup &= ~((3u << 28) | (3u << 30));
    mmio_write(GPIO_PUP_PDN0, pup);
    delay(150);

    // Clear pending interrupts.
    mmio_write(UART0_ICR, 0x7FF);

    // Baud rate divisor for 115200 8N1 assuming a 48MHz UART reference clock
    // (the common default set by the GPU firmware). Re-check this against
    // the real board once the USB-TTL cable is in.
    mmio_write(UART0_IBRD, 26);
    mmio_write(UART0_FBRD, 3);

    // Enable FIFOs, 8 bits, no parity, 1 stop bit.
    mmio_write(UART0_LCRH, (1u << 4) | (3u << 5));

    // Enable UART, TX, RX.
    mmio_write(UART0_CR, (1u << 0) | (1u << 8) | (1u << 9));
}

void serial_putc(char c) {
    while (mmio_read(UART0_FR) & (1u << 5)) {
        // wait while TX FIFO full
    }
    mmio_write(UART0_DR, (unsigned int)c);
}

char serial_getc(void) {
    while (mmio_read(UART0_FR) & (1u << 4)) {
        // wait while RX FIFO empty (UART0_FR.RXFE)
    }
    return (char)(mmio_read(UART0_DR) & 0xFF);
}
