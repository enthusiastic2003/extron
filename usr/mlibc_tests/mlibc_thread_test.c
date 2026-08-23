/* End-to-end pthread test for Extron's mlibc sysdeps. This deliberately uses
 * only ordinary POSIX pthread APIs; the sole raw syscall is gettid(), used to
 * verify that workers are distinct scheduler entities inside one process. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define SYS_GETTID 29
#define SYS_THREAD_JOIN 32
#define SYS_FUTEX_WAIT 33
#define WORKERS 4
#define ITERATIONS 2000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready_cond = PTHREAD_COND_INITIALIZER;
static int ready;
static int release_workers;
static int counter;
static long tids[WORKERS];
static _Thread_local int tls_value;

static long gettid_raw(void) {
    register long x8 __asm__("x8") = SYS_GETTID;
    register long x0 __asm__("x0");
    __asm__ volatile ("svc #0" : "=r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

static long join_tid_raw(long tid) {
    register long x8 __asm__("x8") = SYS_THREAD_JOIN;
    register long x0 __asm__("x0") = tid;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

static long futex_wait_raw(int *word, int expected, long timeout_ms) {
    register long x8 __asm__("x8") = SYS_FUTEX_WAIT;
    register long x0 __asm__("x0") = (long)word;
    register long x1 __asm__("x1") = expected;
    register long x2 __asm__("x2") = timeout_ms;
    __asm__ volatile ("svc #0" : "+r"(x0)
                      : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}

static void *worker(void *argument) {
    intptr_t index = (intptr_t)argument;
    tls_value = 1000 + (int)index;
    tids[index] = gettid_raw();

    pthread_mutex_lock(&lock);
    ready++;
    pthread_cond_signal(&ready_cond);
    while (!release_workers)
        pthread_cond_wait(&ready_cond, &lock);
    pthread_mutex_unlock(&lock);

    for (int i = 0; i < ITERATIONS; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    return (void *)(intptr_t)tls_value;
}

int main(void) {
    pthread_t threads[WORKERS];
    int failures = 0;
    tls_value = 77;

    int futex_word = 1;
    if (futex_wait_raw(&futex_word, 0, 0) != -2) {
        puts("[thread_test] futex mismatch did not return EAGAIN");
        failures++;
    }
    futex_word = 0;
    if (futex_wait_raw(&futex_word, 0, 20) != -3) {
        puts("[thread_test] timed futex wait did not time out");
        failures++;
    }

    puts("[thread_test] creating pthread workers");
    for (intptr_t i = 0; i < WORKERS; i++) {
        int e = pthread_create(&threads[i], NULL, worker, (void *)i);
        if (e) {
            printf("[thread_test] pthread_create(%ld) failed: %d\n", (long)i, e);
            return 1;
        }
    }

    pthread_mutex_lock(&lock);
    while (ready != WORKERS)
        pthread_cond_wait(&ready_cond, &lock);
    release_workers = 1;
    pthread_cond_broadcast(&ready_cond);
    pthread_mutex_unlock(&lock);

    for (intptr_t i = 0; i < WORKERS; i++) {
        void *result = NULL;
        int e = pthread_join(threads[i], &result);
        if (e || (intptr_t)result != 1000 + i) {
            printf("[thread_test] join %ld failed: e=%d result=%ld\n",
                   (long)i, e, (long)(intptr_t)result);
            failures++;
        }
    }

    /* mlibc currently leaves TCB/stack reclamation as a FIXME. Exercise the
     * kernel's explicit join/reap ABI independently after POSIX join has
     * collected each return value. */
    for (int i = 0; i < WORKERS; i++) {
        if (join_tid_raw(tids[i]) != 0) {
            printf("[thread_test] kernel join/reap failed for TID %ld\n", tids[i]);
            failures++;
        }
    }

    if (counter != WORKERS * ITERATIONS) {
        printf("[thread_test] mutex counter=%d expected=%d\n",
               counter, WORKERS * ITERATIONS);
        failures++;
    }
    if (tls_value != 77) {
        printf("[thread_test] main TLS was corrupted: %d\n", tls_value);
        failures++;
    }
    for (int i = 0; i < WORKERS; i++) {
        if (tids[i] <= 0 || tids[i] == gettid_raw())
            failures++;
        for (int j = 0; j < i; j++)
            if (tids[i] == tids[j])
                failures++;
    }

    printf("[thread_test] tids: %ld %ld %ld %ld\n",
           tids[0], tids[1], tids[2], tids[3]);
    printf("[thread_test] counter=%d TLS=%d: %s\n", counter, tls_value,
           failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
