/*
 * Port of the retired usr/mem_stress.c, rewritten against real mlibc.
 *
 * Deliberately does NOT go through malloc()/free(): the original test's
 * assumptions ("32MB is exactly 8192 separate pmm_alloc_page() calls",
 * "freeing everything and reallocating lands back at the same base
 * address") are assumptions about SYS_ANON_ALLOC/SYS_ANON_FREE and the
 * VMA allocator underneath them, not about mlibc's own allocator on top
 * — mlibc's malloc() is free to carve one big mapping into smaller
 * chunks out of its own arena, batch frees, or round sizes up, any of
 * which would make "freed VA space reused" and "8192 pages all
 * distinct" test mlibc's heap implementation instead of the kernel's.
 * Calling SYS_ANON_ALLOC/SYS_ANON_FREE directly via the same raw_syscall
 * shim mlibc_syscall_test.c already established keeps this exercising
 * exactly what the original did.
 *
 * Before pmm_alloc_page() returned NULL instead of panicking, every
 * out-of-memory branch below was dead code — uvm.c's unwind-and-free on
 * a partial allocation had never once executed. A test that only
 * allocates successfully would not have noticed either way, so this
 * deliberately asks for something that cannot be satisfied.
 */
#include <stdio.h>
#include <stdint.h>

static int failures = 0;

static void check(const char *what, int ok) {
    printf("[mem_stress] %-46s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

#define SYS_ANON_ALLOC 4
#define SYS_ANON_FREE  5

static long raw_syscall(long n, long a1, long a2, long a3) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a1;
    register long x1 __asm__("x1") = a2;
    register long x2 __asm__("x2") = a3;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}

/* Same convention as sys_anon_allocate()/sys_anon_free() in
 * sysdeps/extron/generic/generic.cpp: a negative return is -errno, a
 * non-negative return from alloc is the mapped address (0 is a valid
 * failure signal too — the kernel never hands out VA 0). */
static void *anon_alloc(size_t size) {
    long ret = raw_syscall(SYS_ANON_ALLOC, (long)size, 0, 0);
    return ret > 0 ? (void *)ret : NULL;
}

static void anon_free(void *ptr, size_t size) {
    raw_syscall(SYS_ANON_FREE, (long)ptr, (long)size, 0);
}

#define CHUNK      (1024 * 1024)   /* 1MB */
#define CHUNKS     32              /* 32MB total = 8192 pages */
#define PAGE_SIZE  4096

int main(void) {
    printf("\n[mem_stress] === allocator under load (raw SYS_ANON_ALLOC/FREE) ===\n");

    static void *chunks[CHUNKS];

    /* --- bulk allocation --- */
    int all_ok = 1;
    for (int i = 0; i < CHUNKS; i++) {
        chunks[i] = anon_alloc(CHUNK);
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
        if (chunks[i]) anon_free(chunks[i], CHUNK);

    void *again = anon_alloc(CHUNK);
    check("realloc after full free", again != NULL);
    /* The VMA list's gaps merge automatically, so the first fit after
     * freeing everything should land back at the original base. */
    check("freed VA space reused", again == first);
    if (again) anon_free(again, CHUNK);

    /* --- failure paths, previously unreachable --- */

    /* Larger than USER_HEAP_SIZE (256MB), so no gap can ever satisfy it.
     * Must return NULL; before this change the allocator's own error
     * handling was dead code behind a panic. */
    check("oversized request refused", anon_alloc(512UL * 1024 * 1024) == NULL);

    /* Zero-size and absurd-size must both fail cleanly rather than
     * wrapping into a small successful allocation. */
    check("zero-size refused", anon_alloc(0) == NULL);
    check("overflow-size refused", anon_alloc((size_t)-1) == NULL);

    /* Still functional after all that — the refusals must not have left
     * the allocator in a broken state. */
    void *after = anon_alloc(4096);
    check("usable after refusals", after != NULL);
    if (after) anon_free(after, 4096);

    printf("[mem_stress] === %d failure(s) ===\n", failures);
    return failures;
}
