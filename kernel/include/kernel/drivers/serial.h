#ifndef SERIAL_H
#define SERIAL_H

void init_serial(void);
void serial_putc(char c);
char serial_getc(void); /* blocks until a byte arrives */

#endif
