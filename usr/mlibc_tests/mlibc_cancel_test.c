#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static int block_pipe[2];
static int ready_pipe[2];
static volatile int cleanup_count;
static volatile int spin_release;
static volatile int disabled_worker_resumed;

static void cleanup(void *argument) {
    cleanup_count += (int)(intptr_t)argument;
}

static void *blocked_worker(void *argument) {
    (void)argument;
    pthread_cleanup_push(cleanup, (void *)(intptr_t)1);
    write(ready_pipe[1], "r", 1);
    char byte;
    read(block_pipe[0], &byte, 1);
    pthread_cleanup_pop(0);
    return (void *)(intptr_t)77;
}

static void *explicit_point_worker(void *argument) {
    (void)argument;
    pthread_cleanup_push(cleanup, (void *)(intptr_t)10);
    write(ready_pipe[1], "r", 1);
    while (!spin_release)
        __asm__ volatile ("" ::: "memory");
    pthread_testcancel();
    pthread_cleanup_pop(0);
    return (void *)(intptr_t)88;
}

static void *disabled_worker(void *argument) {
    (void)argument;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    pthread_cleanup_push(cleanup, (void *)(intptr_t)100);
    write(ready_pipe[1], "r", 1);
    char byte;
    read(block_pipe[0], &byte, 1);
    disabled_worker_resumed = 1;
    /* Deferred cancellation remains queued after re-enabling until the
     * thread reaches a cancellation point. */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_testcancel();
    pthread_cleanup_pop(0);
    return (void *)(intptr_t)99;
}

static int report(const char *name, int okay) {
    printf("[cancel_test] %-50s %s\n", name, okay ? "PASS" : "FAIL");
    return okay ? 0 : 1;
}

static int wait_ready(void) {
    char byte = 0;
    return read(ready_pipe[0], &byte, 1) == 1 && byte == 'r';
}

int main(void) {
    int failures = 0;
    if (pipe(block_pipe) || pipe(ready_pipe)) {
        puts("[cancel_test] pipe setup failed");
        return 1;
    }

    pthread_t thread;
    void *result = NULL;
    int created = pthread_create(&thread, NULL, blocked_worker, NULL) == 0;
    int ready = created && wait_ready();
    int cancelled = ready && pthread_cancel(thread) == 0;
    int joined = cancelled && pthread_join(thread, &result) == 0;
    failures += report("blocked cancellation point exits as PTHREAD_CANCELED",
                       joined && result == PTHREAD_CANCELED);
    failures += report("pthread cancellation runs cleanup handlers",
                       cleanup_count == 1);

    result = NULL;
    created = pthread_create(&thread, NULL, explicit_point_worker, NULL) == 0;
    ready = created && wait_ready();
    cancelled = ready && pthread_cancel(thread) == 0;
    spin_release = 1;
    joined = cancelled && pthread_join(thread, &result) == 0;
    failures += report("deferred request waits for pthread_testcancel()",
                       joined && result == PTHREAD_CANCELED
                       && cleanup_count == 11);

    result = NULL;
    created = pthread_create(&thread, NULL, disabled_worker, NULL) == 0;
    ready = created && wait_ready();
    cancelled = ready && pthread_cancel(thread) == 0;
    int released = cancelled && write(block_pipe[1], "x", 1) == 1;
    joined = released && pthread_join(thread, &result) == 0;
    failures += report("disabled cancellation remains queued until enabled",
                       joined && result == PTHREAD_CANCELED
                       && disabled_worker_resumed && cleanup_count == 111);

    printf("[cancel_test] === %d failure(s) ===\n", failures);
    return failures ? 1 : 0;
}
