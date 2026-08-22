#ifndef STDLIB_H
#define STDLIB_H
#include <stddef.h>

void *malloc(size_t size);
void *calloc(size_t nobj, size_t size);
void *realloc(void *p, size_t size);
void  free(void *p);

void  exit(int status) __attribute__((noreturn));
void  abort(void)      __attribute__((noreturn));
int   abs(int v);
long  labs(long v);
long  strtol(const char *s, char **end, int base);
int   atoi(const char *s);
long  atol(const char *s);
double atof(const char *s);
char *getenv(const char *name);
/* No shell to run anything with; always fails. */
int   system(const char *cmd);

#endif
