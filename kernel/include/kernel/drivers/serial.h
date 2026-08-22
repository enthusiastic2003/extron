#ifndef SERIAL_H
#define SERIAL_H

void init_serial(void);
void serial_putc(char c);
char serial_getc(void); /* blocks until a byte arrives */
int serial_try_getc(void); /* non-blocking: byte (0-255), or -1 if none pending */
void serial_enable_rx_irq(void); /* unmask UART0_IMSC.RXIM */

#endif
