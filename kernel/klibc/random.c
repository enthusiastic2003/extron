/*
 * Kernel entropy for getrandom(2).
 *
 * x86-64 uses RDRAND when the CPU has it (retried, as the instruction may
 * transiently fail). Everything else — and the whole path on CPUs without
 * RDRAND — falls back to xoshiro256** reseeded from the cycle counter on
 * every call. That fallback is unpredictable to a casual observer but is
 * NOT cryptographically strong; real hardware RNG sources (virtio-rng,
 * RNDR on aarch64) should replace it when drivers for them exist.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <kernel/klibc/string.h>

static uint64_t rng_state[4];
static bool rng_seeded;

static uint64_t cycle_counter(void) {
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

#if defined(__x86_64__)
static int have_rdrand = -1;
static bool rdrand64(uint64_t *out) {
    if (have_rdrand < 0) {
        uint32_t a = 1, b, c, d;
        __asm__ volatile ("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
        have_rdrand = (c >> 30) & 1;
    }
    if (!have_rdrand)
        return false;
    for (int i = 0; i < 10; i++) {
        uint64_t v; unsigned char ok;
        __asm__ volatile ("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) : : "cc");
        if (ok) { *out = v; return true; }
    }
    return false;
}
#else
static bool rdrand64(uint64_t *out) { (void)out; return false; }
#endif

static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static uint64_t xoshiro_next(void) {
    uint64_t *s = rng_state;
    uint64_t r = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t; s[3] = rotl(s[3], 45);
    return r;
}

void kernel_random_fill(void *buf, size_t len) {
    /* Fold fresh cycle-counter bits into the state on every call. */
    uint64_t seed = cycle_counter() ^ (rng_seeded ? rng_state[0] : 0);
    for (int i = 0; i < 4; i++)
        rng_state[i] ^= splitmix64(&seed);
    rng_seeded = true;

    uint8_t *out = buf;
    while (len) {
        uint64_t v;
        if (!rdrand64(&v))
            v = xoshiro_next();
        size_t n = len < 8 ? len : 8;
        memcpy(out, &v, n);
        out += n; len -= n;
    }
}
