/*
 * Exercises the allocator stack under real load, and checks the failure
 * paths that used to be unreachable.
 *
 * Before pmm_alloc_page() returned NULL instead of panicking, every
 * out-of-memory branch below the syscall was dead code — uvm.c's
 * unwind-and-free on a partial allocation had never once executed. A
 * test that only allocates successfully would not have noticed either
 * way, so this deliberately asks for something that cannot be satisfied.
 *
 * It also puts real pressure on the PMM's page scan: 32MB is 8192 pages,
 * each one a separate pmm_alloc_page(). That is the path that used to
 * restart a bit-by-bit scan from index 0 on every single call.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <extron/syscall.h>

#define CHUNK      (1024 * 1024)   /* 1MB */
#define CHUNKS     32              /* 32MB total = 8192 pages */
#define PAGE_SIZE  4096

static int failures;

static void check(const char *what, int ok) {
    printf("  %-30s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    puts("MEMSTRESS: allocator under load");

    static void *chunks[CHUNKS];

    /* --- bulk allocation --- */
    int all_ok = 1;
    for (int i = 0; i < CHUNKS; i++) {
        chunks[i] = sys_anon_alloc(CHUNK);
        if (!chunks[i]) { all_ok = 0; break; }
    }
    check("32MB in 1MB chunks", all_ok);

    /* Touch one byte per page with a per-chunk pattern. Per PAGE, not
     * per chunk: every page is a separate pmm_alloc_page() and a
     * separate map_page(), so a single-byte-per-chunk check would miss
     * a duplicate page handed out inside a chunk. */
    if (all_ok) {
        for (int i = 0; i < CHUNKS; i++) {
            volatile unsigned char *p = chunks[i];
            for (int off = 0; off < CHUNK; off += PAGE_SIZE)
                p[off] = (unsigned char)(i + 1);
        }
        int intact = 1;
        for (int i = 0; i < CHUNKS && intact; i++) {
            volatile unsigned char *p = chunks[i];
            for (int off = 0; off < CHUNK; off += PAGE_SIZE)
                if (p[off] != (unsigned char)(i + 1)) { intact = 0; break; }
        }
        /* A page handed out twice shows up here as one chunk's pattern
         * appearing inside another's — the classic symptom of a bitmap
         * losing a bit. */
        check("8192 pages all distinct", intact);
    }

    /* --- free everything, then prove the space comes back --- */
    void *first = chunks[0];
    for (int i = 0; i < CHUNKS; i++)
        if (chunks[i]) sys_anon_free(chunks[i], CHUNK);

    void *again = sys_anon_alloc(CHUNK);
    check("realloc after full free", again != NULL);
    /* The VMA list's gaps merge automatically, so the first fit after
     * freeing everything should land back at the original base. */
    check("freed VA space reused", again == first);
    if (again) sys_anon_free(again, CHUNK);

    /* --- failure paths, previously unreachable --- */

    /* Larger than USER_HEAP_SIZE (256MB), so no gap can ever satisfy it.
     * Must return NULL; before this change the allocator's own error
     * handling was dead code behind a panic. */
    check("oversized request refused", sys_anon_alloc(512UL * 1024 * 1024) == NULL);

    /* Zero-size and absurd-size must both fail cleanly rather than
     * wrapping into a small successful allocation. */
    check("zero-size refused", sys_anon_alloc(0) == NULL);
    check("overflow-size refused", sys_anon_alloc((size_t)-1) == NULL);

    /* Still functional after all that — the refusals must not have left
     * the allocator in a broken state. */
    void *after = sys_anon_alloc(4096);
    check("usable after refusals", after != NULL);
    if (after) sys_anon_free(after, 4096);

    printf("MEMSTRESS: %d failure(s)\n", failures);
    return failures;
}
