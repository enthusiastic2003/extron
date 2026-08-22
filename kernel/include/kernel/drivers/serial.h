#ifndef SERIAL_H
#define SERIAL_H

/* The BCM2711 peripheral pages this driver actually touches: GPIO
 * (0xFE200000, pin mux + pull-ups) and PL011 UART0 (0xFE201000). Two
 * adjacent 4KB pages. Exported so init_paging() can give them a
 * Device-memory mapping in the kernel's own table without duplicating
 * the addresses — uart.c stays the one place that knows them. */
#define SERIAL_MMIO_PHYS 0xFE200000ULL
#define SERIAL_MMIO_SIZE 0x2000ULL

void init_serial(void);

/* Repoint register access at the high-half alias of the block above.
 * Called once by init_paging(), after the mapping exists. */
void serial_remap_to_hhdm(void);
void serial_putc(char c);
char serial_getc(void); /* blocks until a byte arrives */
int serial_try_getc(void); /* non-blocking: byte (0-255), or -1 if none pending */
void serial_enable_rx_irq(void); /* unmask UART0_IMSC.RXIM */

#endif
