#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#define WORKERS 4
#define ITERATIONS 64

static pthread_mutex_t barrier_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t barrier_cond = PTHREAD_COND_INITIALIZER;
static int barrier_ready;
static int barrier_release;
static int failures;

static void fail(void) {
    __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
}

static void *worker(void *argument) {
    intptr_t id = (intptr_t)argument;
    void *handle = dlopen("/opt/tests/libextron_rtld_test.so", RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fail();
        return NULL;
    }

    if (dlsym(handle, "missing_symbol_for_threaded_dlerror") != NULL)
        fail();

    pthread_mutex_lock(&barrier_lock);
    barrier_ready++;
    pthread_cond_broadcast(&barrier_cond);
    while (!barrier_release)
        pthread_cond_wait(&barrier_cond, &barrier_lock);
    pthread_mutex_unlock(&barrier_lock);

    /* Every worker set its own error before the barrier. A process-global
     * dlerror slot would let only one worker consume an error here. */
    if (!dlerror())
        fail();
    if (dlclose(handle) != 0)
        fail();

    for (int iteration = 0; iteration < ITERATIONS; iteration++) {
        handle = dlopen("/opt/tests/libextron_rtld_test.so", RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            fail();
            continue;
        }

        int (*value)(void) = (int (*)(void))dlsym(handle, "extron_dso_value");
        int *tls = (int *)dlsym(handle, "extron_dso_tls");
        if (!value || !tls) {
            fail();
        } else {
            int expected_tls = 1000 + (int)id;
            *tls = expected_tls;
            if (value() != 22 + expected_tls)
                fail();
        }

        if (dlclose(handle) != 0)
            fail();
    }
    return NULL;
}

int main(void) {
    pthread_t threads[WORKERS];
    puts("[dlthread_test] concurrent dlopen/dlsym/TLS/dlclose stress");

    for (intptr_t i = 0; i < WORKERS; i++) {
        if (pthread_create(&threads[i], NULL, worker, (void *)i) != 0) {
            puts("[dlthread_test] pthread_create failed");
            return 1;
        }
    }

    pthread_mutex_lock(&barrier_lock);
    while (barrier_ready != WORKERS)
        pthread_cond_wait(&barrier_cond, &barrier_lock);
    barrier_release = 1;
    pthread_cond_broadcast(&barrier_cond);
    pthread_mutex_unlock(&barrier_lock);

    for (int i = 0; i < WORKERS; i++) {
        if (pthread_join(threads[i], NULL) != 0)
            fail();
    }

    printf("[dlthread_test] %d operations across %d workers: %s (%d failures)\n",
           WORKERS * ITERATIONS, WORKERS, failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
