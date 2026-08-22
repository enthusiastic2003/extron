#include <stdlib.h>
#include <extron/syscall.h>
#include <sys/time.h>
#include <stdint.h>

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

/* No environment on this system — every lookup misses, which is what
 * DOOM's optional DOOMWADDIR/HOME probes expect to happen. */
char *getenv(const char *name) {
    (void)name;
    return NULL;
}

/* Only used by m_config.c for float-valued settings, none of which DOOM
 * ships a default for. Handles sign, integer part and fraction; no
 * exponent, which nothing here writes. */
double atof(const char *s) {
    while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
    int neg = 0;
    if (*s == '+' || *s == '-') neg = (*s++ == '-');

    double v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    if (*s == '.') {
        s++;
        double scale = 0.1;
        while (*s >= '0' && *s <= '9') { v += (*s++ - '0') * scale; scale *= 0.1; }
    }
    return neg ? -v : v;
}

/* Nowhere to create a directory. DOOM calls this for its save folder and
 * copes with failure. */
int mkdir(const char *path, unsigned mode) {
    (void)path; (void)mode;
    return -1;
}

int gettimeofday(struct timeval *tv, struct timezone *tz) {
    (void)tz;
    if (tv) {
        uint64_t ms = sys_uptime_ms();
        tv->tv_sec  = (long)(ms / 1000);
        tv->tv_usec = (long)((ms % 1000) * 1000);
    }
    return 0;
}

int usleep(unsigned us) {
    sys_sleep(us / 1000000u, (long)(us % 1000000u) * 1000L);
    return 0;
}

unsigned sleep(unsigned s) {
    sys_sleep(s, 0);
    return 0;
}

int system(const char *cmd) {
    (void)cmd;
    return -1;
}
