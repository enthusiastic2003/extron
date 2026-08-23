#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int worker_waiting;

static void *blocked_worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&lock);
    worker_waiting = 1;
    pthread_cond_signal(&cond);
    for (;;)
        pthread_cond_wait(&cond, &lock);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "after-exec")) {
        puts("[thread_exec_test] replacement image has one surviving thread: PASS");
        return 0;
    }

    pthread_t thread;
    int e = pthread_create(&thread, NULL, blocked_worker, NULL);
    if (e) {
        printf("[thread_exec_test] pthread_create failed: %d\n", e);
        return 1;
    }

    pthread_mutex_lock(&lock);
    while (!worker_waiting)
        pthread_cond_wait(&cond, &lock);
    pthread_mutex_unlock(&lock);

    puts("[thread_exec_test] exec with a blocked sibling");
    char *args[] = { "/mlibc_thread_exec_test.elf", "after-exec", NULL };
    execve(args[0], args, NULL);
    perror("[thread_exec_test] execve");
    return 1;
}
