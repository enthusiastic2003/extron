#ifndef UNISTD_H
#define UNISTD_H
#include <stddef.h>
/* Just enough of the POSIX surface for DOOM's setup paths. There is no
 * filesystem to speak of and no tty layer, so these answer honestly
 * rather than pretending. */
static inline int isatty(int fd) { (void)fd; return 1; }   /* serial console */
int usleep(unsigned us);
unsigned sleep(unsigned s);
int mkdir(const char *path, unsigned mode);
#endif
