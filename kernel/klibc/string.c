#include <stddef.h>
#include <kernel/klibc/string.h>

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }

    return dst;
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;

    for (size_t i = 0; i < n; i++) {
        p[i] = (unsigned char)c;
    }

    return s;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d < s) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = n; i != 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }

    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p1 = (const unsigned char *)a;
    const unsigned char *p2 = (const unsigned char *)b;

    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return (int)p1[i] - (int)p2[i];
        }
    }

    return 0;
}