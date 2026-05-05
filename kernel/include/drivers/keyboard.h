#ifndef DRIVERS_KEYBOARD_H
#define DRIVERS_KEYBOARD_H

#include <stdint.h>

void keyboard_init();
char keyboard_get_char(); // Non-blocking: returns 0 if no key pressed
char keyboard_wait_char(); // Blocking

#endif
