#ifndef STDIO_H
#define STDIO_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

int printf(const char *fmt, ...)  __attribute__((format(printf, 1, 2)));
int vprintf(const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...)
        __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int puts(const char *s);
int putchar(int c);

/*
 * FILE, backed by the initrd.
 *
 * Read-only, and deliberately so: the initrd is the kernel's own copy,
 * shared and mapped without write permission. fopen() for reading maps
 * the named file into this process (SYS_MAP_INITRD — a view, not a
 * copy, so opening a multi-megabyte WAD costs page-table entries and
 * nothing else). fopen() for writing succeeds but discards, because
 * DOOM writes a config file on exit and treating that as fatal would
 * mean it could never quit cleanly.
 *
 * The point of implementing FILE at all rather than patching DOOM's
 * w_file_stdc.c is that DOOM's own file code then works unmodified.
 */
typedef struct {
    const unsigned char *data;   /* NULL for a write handle */
    unsigned long        size;
    unsigned long        pos;
    int                  eof;
    int                  writable;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define EOF      (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t n, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t n, FILE *f);
int    fseek(FILE *f, long off, int whence);
long   ftell(FILE *f);
void   rewind(FILE *f);
int    feof(FILE *f);
int    ferror(FILE *f);
int    fflush(FILE *f);
int    fgetc(FILE *f);
char  *fgets(char *buf, int n, FILE *f);
int    fputc(int c, FILE *f);
int    fputs(const char *s, FILE *f);
int    fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int    vfprintf(FILE *f, const char *fmt, va_list ap);
int    remove(const char *path);
int    rename(const char *a, const char *b);

int    sscanf(const char *str, const char *fmt, ...);

#endif
