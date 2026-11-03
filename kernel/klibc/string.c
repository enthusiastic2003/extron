#include <stddef.h>
#include <stdint.h>
#include <kernel/klibc/string.h>

#if defined(__x86_64__)
/*
 * x86-64: wide, hardware-accelerated block operations. `rep movsq/stosq`
 * move 8 bytes per iteration architecturally and, on CPUs with fast-string
 * microcode (ERMS/FSRM, i.e. nearly everything since Ivy Bridge), run as
 * full cache-line-wide transfers; QEMU's TCG executes each as one helper
 * call instead of one translated loop iteration per byte. Explicit
 * SSE/AVX is deliberately not used: the kernel is built
 * -mgeneral-regs-only so user x87/SSE state survives every syscall and IRQ
 * untouched (context_switch is the only place that saves it), and touching
 * XMM here would silently corrupt user floating point.
 */
typedef uint64_t __attribute__((may_alias, aligned(1))) u64u;

void *memcpy(void *dst, const void *src, size_t n) {
    void *ret = dst;
    size_t q = n >> 3;
    __asm__ volatile ("rep movsq" : "+D"(dst), "+S"(src), "+c"(q) :: "memory");
    n &= 7;
    __asm__ volatile ("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) :: "memory");
    return ret;
}

void *memset(void *s, int c, size_t n) {
    void *ret = s;
    uint64_t pat = (uint64_t)(unsigned char)c * 0x0101010101010101ULL;
    size_t q = n >> 3;
    __asm__ volatile ("rep stosq" : "+D"(s), "+c"(q) : "a"(pat) : "memory");
    n &= 7;
    __asm__ volatile ("rep stosb" : "+D"(s), "+c"(n) : "a"(pat) : "memory");
    return ret;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d <= s || d >= s + n)
        return memcpy(dst, src, n);          /* forward copy is overlap-safe */
    /* Overlapping with dst above src: copy backwards, 8 bytes at a time
     * (`std; rep movs` would disable fast-string and crawl). */
    d += n; s += n;
    while (n >= 8) {
        d -= 8; s -= 8; n -= 8;
        *(u64u *)d = *(const u64u *)s;
    }
    while (n--) *--d = *--s;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p1 = (const unsigned char *)a;
    const unsigned char *p2 = (const unsigned char *)b;
    while (n >= 8 && *(const u64u *)p1 == *(const u64u *)p2) { p1 += 8; p2 += 8; n -= 8; }
    for (; n; n--, p1++, p2++)
        if (*p1 != *p2) return (int)*p1 - (int)*p2;
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while ((uintptr_t)p & 7) { if (!*p) return (size_t)(p - s); p++; }
    /* Aligned 8-byte reads never cross a page, so scanning past the NUL
     * within the final word is safe. Classic has-zero-byte test. */
    for (;;) {
        uint64_t w = *(const uint64_t *)p;
        if ((w - 0x0101010101010101ULL) & ~w & 0x8080808080808080ULL) break;
        p += 8;
    }
    while (*p) p++;
    return (size_t)(p - s);
}
#else
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

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

#endif

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char a = (unsigned char)s1[i];
        unsigned char b = (unsigned char)s2[i];
        if (a != b || !a) return (int)a - (int)b;
    }
    return 0;
}

char *strchr(const char *s, int c) {
    do {
        if (*s == (char)c) return (char *)s;
    } while (*s++);
    return NULL;
}
