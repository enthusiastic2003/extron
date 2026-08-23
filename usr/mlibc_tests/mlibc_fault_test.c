/* Verifies that an EL0 fault kills only the faulting process, reports to its
 * direct parent with POSIX wait status, and also tears down sibling threads. */
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

__attribute__((noinline, noreturn))
static void cause_data_abort(void) {
    uintptr_t address = 0;
    __asm__ volatile ("ldr x9, [%0]" :: "r"(address) : "x9", "memory");
    __builtin_unreachable();
}

static void *faulting_worker(void *unused) {
    (void)unused;
    cause_data_abort();
}

static int expect_fault(pid_t child, const char *which) {
    int status = 0;
    pid_t reaped = wait(&status);
    if (reaped != child || !WIFSIGNALED(status)
            || WTERMSIG(status) != SIGSEGV) {
        printf("[fault_test] %s: child=%ld reaped=%ld status=0x%x FAIL\n",
               which, (long)child, (long)reaped, status);
        return 1;
    }
    printf("[fault_test] %s: direct parent received SIGSEGV PASS\n", which);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "self"))
        cause_data_abort();

    int failures = 0;

    pid_t child = fork();
    if (child < 0)
        return 1;
    if (!child)
        cause_data_abort();
    failures += expect_fault(child, "main-thread fault");

    child = fork();
    if (child < 0)
        return 1;
    if (!child) {
        pthread_t worker;
        if (pthread_create(&worker, NULL, faulting_worker, NULL))
            _Exit(2);
        /* A worker fault must terminate this blocked main thread too. */
        pthread_join(worker, NULL);
        _Exit(3);
    }
    failures += expect_fault(child, "worker-thread fault");

    printf("[fault_test] parent continued after both children: %s\n",
           failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
