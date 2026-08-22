/*
 * First C payload on this kernel, and the proof that the userspace
 * toolchain works end to end: crt0 -> main -> libc -> syscalls -> exit.
 *
 * Every check here is something DOOM will lean on. malloc in particular
 * is the load-bearing one — it's the SAME allocator the kernel runs
 * (kernel/mm/liballoc.c), compiled a second time against
 * usr/include/liballoc_config.h with its page hooks pointed at
 * SYS_ANON_ALLOC instead of vmm_alloc_pages.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <extron/syscall.h>

static int failures;

static void check(const char *what, int ok) {
    printf("  %-28s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    puts("LIBC: userspace C is alive");

    /* --- string.h --- */
    char buf[64];
    strcpy(buf, "doom");
    check("strcpy/strlen", strlen(buf) == 4 && !strcmp(buf, "doom"));

    strncpy(buf, "ab", 8);
    /* strncpy's zero-fill, which DOOM relies on for fixed-width WAD
     * lump names — a plain strcpy would leave buf[2..7] as garbage. */
    check("strncpy zero-pads", buf[2] == 0 && buf[7] == 0);

    check("strcasecmp", strcasecmp("MAP01", "map01") == 0);
    check("strchr/strrchr", strchr("a/b/c", '/')[1] == 'b' &&
                            strrchr("a/b/c", '/')[1] == 'c');
    check("strstr", strstr("PWAD_HEADER", "HEAD") != NULL &&
                    strstr("abc", "xyz") == NULL);

    /* Char-by-char, not a string literal: this is exactly 8 bytes with no
     * room for a NUL, and it's compared with memcmp rather than strcmp. */
    char ov[8] = { '1', '2', '3', '4', '5', '6', '7', '8' };
    memmove(ov + 1, ov, 7);           /* overlapping, dst above src */
    check("memmove overlap", memcmp(ov, "11234567", 8) == 0);

    /* --- stdlib.h --- */
    check("atoi/strtol", atoi("-42") == -42 && strtol("ff", NULL, 16) == 255);
    char *end;
    strtol("zz", &end, 10);
    /* On no conversion strtol must return the ORIGINAL pointer, which is
     * how callers detect failure at all. */
    check("strtol no-conversion", end != NULL && end[0] == 'z');

    /* --- stdio.h --- */
    int n = snprintf(buf, sizeof buf, "%d|%05d|%x|%s|%c|%%", -7, 42, 48879, "ok", 'Z');
    int fmt_ok = !strcmp(buf, "-7|00042|beef|ok|Z|%") && n == 20;
    if (!fmt_ok) printf("    got \"%s\" (n=%d)\n", buf, n);
    check("snprintf formatting", fmt_ok);

    /* Left-justify with a width, on a numeric and after other output —
     * the combination that catches padding computed against the running
     * total rather than this conversion's own length. */
    snprintf(buf, sizeof buf, "[%-6d][%6d]", 42, 42);
    check("snprintf width/justify", !strcmp(buf, "[42    ][    42]"));

    char tiny[5];
    n = snprintf(tiny, sizeof tiny, "abcdefgh");
    /* C99: return the length it WOULD have written, and still terminate. */
    check("snprintf truncation", n == 8 && !strcmp(tiny, "abcd"));

    /* --- malloc: the shared liballoc, on user pages --- */
    char *a = malloc(100);
    char *b = malloc(9000);           /* spans more than one page */
    check("malloc non-null/distinct", a && b && a != b);

    memset(a, 'A', 100);
    memset(b, 'B', 9000);
    int intact = 1;
    for (int i = 0; i < 100; i++)  if (a[i] != 'A') intact = 0;
    for (int i = 0; i < 9000; i++) if (b[i] != 'B') intact = 0;
    check("malloc regions isolated", intact);

    free(a);
    char *c = malloc(100);
    check("free then reuse", c != NULL);

    int *z = calloc(64, sizeof(int));
    int zeroed = z != NULL;
    for (int i = 0; z && i < 64; i++) if (z[i]) zeroed = 0;
    check("calloc zeroes", zeroed);

    free(b);
    free(c);
    free(z);

    /* --- syscalls straight through --- */
    uint64_t t0 = sys_uptime_ms();
    sys_sleep(0, 200000000L);          /* 200ms */
    uint64_t dt = sys_uptime_ms() - t0;
    check("sleep+uptime agree", dt >= 150 && dt <= 400);

    /* --- SYS_MAP_INITRD: a view of the initrd, not a copy --- */
    size_t wsize = 0;
    const char *w = sys_map_initrd("hello.txt", &wsize);
    check("map_initrd returns data", w != NULL && wsize == 22);
    check("map_initrd contents", w && !memcmp(w, "Hello from the initrd!", 22));

    /* Non-existent name must fail cleanly rather than mapping something
     * arbitrary — the failure mode that would matter most for a WAD. */
    size_t nsize = 12345;
    check("map_initrd rejects missing", sys_map_initrd("nope.wad", &nsize) == NULL);

    printf("LIBC: %d failure(s)\n", failures);
    return failures;
}
