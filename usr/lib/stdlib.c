#include <stdlib.h>
#include <extron/syscall.h>

/* malloc/free/calloc/realloc come from the shared allocator
 * (kernel/mm/liballoc.c compiled against usr/include/liballoc_config.h);
 * this file is the rest of stdlib. */

void exit(int status) {
    sys_exit(status);
}

void abort(void) {
    sys_exit(134);   /* 128 + SIGABRT, the conventional shell encoding */
}

int abs(int v) {
    return v < 0 ? -v : v;
}

long labs(long v) {
    return v < 0 ? -v : v;
}

static int isspace_(int c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

long strtol(const char *s, char **end, int base) {
    const char *p = s;
    while (isspace_((unsigned char)*p)) p++;

    int neg = 0;
    if (*p == '+' || *p == '-') neg = (*p++ == '-');

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = (*p == '0') ? 8 : 10;
    }

    unsigned long acc = 0;
    int any = 0;
    for (;; p++) {
        int c = (unsigned char)*p, d;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        acc = acc * (unsigned long)base + (unsigned long)d;
        any = 1;
    }

    /* Per C: on no conversion, *end gets the ORIGINAL string, not where
     * parsing stopped — callers use that to detect failure. */
    if (end) *end = (char *)(any ? p : s);
    return neg ? -(long)acc : (long)acc;
}

int atoi(const char *s) {
    return (int)strtol(s, NULL, 10);
}

long atol(const char *s) {
    return strtol(s, NULL, 10);
}
