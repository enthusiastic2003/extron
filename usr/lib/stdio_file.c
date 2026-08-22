#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <extron/syscall.h>

int errno;

/* Console handles. They carry no data pointer — writes go straight out
 * SYS_WRITE and reads are not supported, which is enough for DOOM: it
 * writes diagnostics to stdout/stderr and never reads stdin through
 * stdio (keys come from the mapped input ring instead). */
static FILE console_out = { 0, 0, 0, 0, 1 };
static FILE console_err = { 0, 0, 0, 0, 1 };
static FILE console_in  = { 0, 0, 0, 0, 0 };

FILE *stdout = &console_out;
FILE *stderr = &console_err;
FILE *stdin  = &console_in;

static int is_console(FILE *f) {
    return f == &console_out || f == &console_err || f == &console_in;
}

FILE *fopen(const char *path, const char *mode) {
    if (!path || !mode) {
        errno = EINVAL;
        return NULL;
    }

    int writing = (mode[0] == 'w' || mode[0] == 'a' ||
                   (mode[0] == 'r' && strchr(mode, '+') != NULL));

    if (writing) {
        /* Accepted and discarded. DOOM writes default.cfg when it exits;
         * failing that would turn a clean quit into an error path, and
         * there is nowhere to put it anyway — the initrd is read-only. */
        FILE *f = calloc(1, sizeof(FILE));
        if (!f) { errno = ENOMEM; return NULL; }
        f->writable = 1;
        return f;
    }

    size_t size = 0;
    const void *data = sys_map_initrd(path, &size);
    if (!data) {
        errno = ENOENT;
        return NULL;
    }

    FILE *f = calloc(1, sizeof(FILE));
    if (!f) { errno = ENOMEM; return NULL; }
    f->data = (const unsigned char *)data;
    f->size = size;
    return f;
}

int fclose(FILE *f) {
    if (!f || is_console(f)) return 0;
    /* The mapping stays. Unmapping would need the base VA and length
     * threaded back through, and a process that opens the WAD keeps it
     * open for its whole life anyway. */
    free(f);
    return 0;
}

size_t fread(void *ptr, size_t size, size_t n, FILE *f) {
    if (!f || !f->data || size == 0 || n == 0) return 0;

    size_t want = size * n;
    size_t left = f->size - f->pos;
    if (want > left) {
        want = left;
        f->eof = 1;
    }
    memcpy(ptr, f->data + f->pos, want);
    f->pos += want;
    return want / size;
}

size_t fwrite(const void *ptr, size_t size, size_t n, FILE *f) {
    if (!f) return 0;
    if (f == &console_out || f == &console_err) {
        sys_write(f == &console_err ? 2 : 1, ptr, size * n);
        return n;
    }
    return n;   /* discarded — see fopen() */
}

int fseek(FILE *f, long off, int whence) {
    if (!f || !f->data) return -1;
    long base = (whence == SEEK_CUR) ? (long)f->pos
              : (whence == SEEK_END) ? (long)f->size
              : 0;
    long target = base + off;
    if (target < 0 || (unsigned long)target > f->size) return -1;
    f->pos = (unsigned long)target;
    f->eof = 0;
    return 0;
}

long ftell(FILE *f)            { return f ? (long)f->pos : -1; }
void rewind(FILE *f)           { if (f) { f->pos = 0; f->eof = 0; } }
int  feof(FILE *f)             { return f ? f->eof : 1; }
int  ferror(FILE *f)           { (void)f; return 0; }
int  fflush(FILE *f)           { (void)f; return 0; }

int fgetc(FILE *f) {
    unsigned char c;
    if (fread(&c, 1, 1, f) != 1) return EOF;
    return c;
}

char *fgets(char *buf, int n, FILE *f) {
    if (n <= 0) return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    buf[i] = '\0';
    return buf;
}

int fputc(int c, FILE *f) {
    char ch = (char)c;
    fwrite(&ch, 1, 1, f);
    return c;
}

int fputs(const char *s, FILE *f) {
    fwrite(s, 1, strlen(s), f);
    return 0;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char buf[512];
    int r = vsnprintf(buf, sizeof buf, fmt, ap);
    size_t n = (size_t)r < sizeof buf - 1 ? (size_t)r : sizeof buf - 1;
    fwrite(buf, 1, n, f);
    return r;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int remove(const char *path)             { (void)path; return -1; }
int rename(const char *a, const char *b) { (void)a; (void)b; return -1; }

/* Only the conversions DOOM actually uses (%d and %s, for -width/-height
 * style arguments). Anything else is a silent no-match rather than a
 * wrong parse. */
int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int matched = 0;

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            while (*str == ' ' || *str == '\t') str++;
            if (*fmt == 'd' || *fmt == 'i') {
                char *end;
                long v = strtol(str, &end, 10);
                if (end == str) break;
                *va_arg(ap, int *) = (int)v;
                str = end;
                matched++;
            } else if (*fmt == 'u') {
                char *end;
                long v = strtol(str, &end, 10);
                if (end == str) break;
                *va_arg(ap, unsigned *) = (unsigned)v;
                str = end;
                matched++;
            } else if (*fmt == 's') {
                char *out = va_arg(ap, char *);
                while (*str && *str != ' ' && *str != '\t' && *str != '\n') *out++ = *str++;
                *out = '\0';
                matched++;
            } else {
                break;
            }
            fmt++;
        } else if (*fmt == ' ') {
            while (*str == ' ' || *str == '\t') str++;
            fmt++;
        } else {
            if (*str != *fmt) break;
            str++; fmt++;
        }
    }

    va_end(ap);
    return matched;
}
