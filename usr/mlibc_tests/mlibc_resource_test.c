#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
static volatile uint64_t work_sink;

static void check(const char *what, int ok) {
    printf("[resource_test] %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static uint64_t timeval_us(struct timeval value) {
    return (uint64_t)value.tv_sec * 1000000ULL + (uint64_t)value.tv_usec;
}

static uint64_t usage_us(const struct rusage *usage) {
    return timeval_us(usage->ru_utime) + timeval_us(usage->ru_stime);
}

static uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000ULL + (uint64_t)now.tv_nsec / 1000000ULL;
}

static void burn_cpu(unsigned milliseconds) {
    uint64_t end = monotonic_ms() + milliseconds;
    do {
        for (unsigned i = 0; i < 200000; i++)
            work_sink = work_sink * 1664525U + i + 1013904223U;
    } while (monotonic_ms() < end);
}

static void *thread_burn(void *argument) {
    burn_cpu((unsigned)(uintptr_t)argument);
    return NULL;
}

static int exec_limit_mode(const char *expected_text) {
    struct rlimit limit;
    unsigned long expected = strtoul(expected_text, NULL, 10);
    return getrlimit(RLIMIT_NOFILE, &limit) == 0
        && limit.rlim_cur == expected ? 0 : 90;
}

static void test_limit_reporting(void) {
    struct rlimit files, stack, core;
    struct rusage invalid_usage;
    check("getrlimit reports RLIMIT_NOFILE",
          getrlimit(RLIMIT_NOFILE, &files) == 0
          && files.rlim_cur == 32 && files.rlim_max == 32);
    check("sysconf(_SC_OPEN_MAX) matches RLIMIT_NOFILE",
          sysconf(_SC_OPEN_MAX) == (long)files.rlim_cur);
    check("fixed 128 KiB process stack is reported truthfully",
          getrlimit(RLIMIT_STACK, &stack) == 0
          && stack.rlim_cur == 128 * 1024 && stack.rlim_max == 128 * 1024);
    check("absence of core dumps is reported as a zero limit",
          getrlimit(RLIMIT_CORE, &core) == 0
          && core.rlim_cur == 0 && core.rlim_max == 0);
    check("setting an unchanged fixed stack limit succeeds",
          setrlimit(RLIMIT_STACK, &stack) == 0);

    errno = 0;
    check("invalid resource is rejected with EINVAL",
          getrlimit(RLIMIT_NLIMITS, &files) == -1 && errno == EINVAL);
    errno = 0;
    check("getrlimit rejects an unmapped output pointer",
          getrlimit(RLIMIT_NOFILE, (struct rlimit *)1) == -1
          && errno == EFAULT);
    errno = 0;
    check("getrusage rejects an invalid scope",
          getrusage(12345, &invalid_usage) == -1 && errno == EINVAL);
    struct rlimit reversed = { .rlim_cur = 2, .rlim_max = 1 };
    errno = 0;
    check("soft limit above hard limit is rejected",
          setrlimit(RLIMIT_NOFILE, &reversed) == -1 && errno == EINVAL);
    struct rlimit finite_cpu = { .rlim_cur = 1, .rlim_max = RLIM_INFINITY };
    errno = 0;
    check("unenforced finite CPU limit is explicitly unsupported",
          setrlimit(RLIMIT_CPU, &finite_cpu) == -1 && errno == ENOTSUP);
}

static void test_descriptor_enforcement(void) {
    struct rlimit original;
    getrlimit(RLIMIT_NOFILE, &original);
    int existing = open("/opt/tests/hello.txt", O_RDONLY);
    check("open descriptor used for limit test", existing >= 3);

    struct rlimit reduced = { .rlim_cur = (rlim_t)(existing + 1),
                              .rlim_max = original.rlim_max };
    check("lower RLIMIT_NOFILE soft limit", setrlimit(RLIMIT_NOFILE, &reduced) == 0);
    errno = 0;
    int extra = open("/opt/tests/hello.txt", O_RDONLY);
    check("open at the descriptor limit fails with EMFILE",
          extra == -1 && errno == EMFILE);
    char byte = 0;
    check("descriptor opened before lowering remains usable",
          read(existing, &byte, 1) == 1 && byte == 'H');
    struct rlimit below_existing = {
        .rlim_cur = (rlim_t)existing, .rlim_max = original.rlim_max
    };
    check("lowering a limit does not invalidate dup2(fd, fd)",
          setrlimit(RLIMIT_NOFILE, &below_existing) == 0
          && dup2(existing, existing) == existing);
    setrlimit(RLIMIT_NOFILE, &reduced);
    errno = 0;
    check("dup2 destination at the soft limit is rejected",
          dup2(0, (int)reduced.rlim_cur) == -1 && errno == EBADF);
    errno = 0;
    check("F_DUPFD minimum at the soft limit is rejected",
          fcntl(0, F_DUPFD, (int)reduced.rlim_cur) == -1 && errno == EINVAL);
    close(existing);
    existing = open("/opt/tests/hello.txt", O_RDONLY);
    check("a slot below the soft limit can be reused", existing >= 0);
    close(existing);

    struct rlimit one_slot = { .rlim_cur = 4, .rlim_max = original.rlim_max };
    setrlimit(RLIMIT_NOFILE, &one_slot);
    int pipefd[2];
    errno = 0;
    check("pipe fails atomically when fewer than two slots remain",
          pipe(pipefd) == -1 && errno == EMFILE);
    check("restore original descriptor soft limit",
          setrlimit(RLIMIT_NOFILE, &original) == 0);
}

static void test_inheritance_and_permissions(const char *self) {
    struct rlimit original;
    getrlimit(RLIMIT_NOFILE, &original);
    struct rlimit inherited = { .rlim_cur = 9, .rlim_max = original.rlim_max };
    setrlimit(RLIMIT_NOFILE, &inherited);
    pid_t child = fork();
    if (child == 0) {
        struct rlimit observed;
        if (getrlimit(RLIMIT_NOFILE, &observed) != 0 || observed.rlim_cur != 9)
            _exit(91);
        struct rlimit private_limit = { .rlim_cur = 8, .rlim_max = observed.rlim_max };
        if (setrlimit(RLIMIT_NOFILE, &private_limit) != 0)
            _exit(92);
        char *const args[] = { (char *)self, "--exec-limit", "8", NULL };
        execve(self, args, environ);
        _exit(93);
    }
    int status = 0;
    check("fork inherits and exec preserves resource limits",
          waitpid(child, &status, 0) == child
          && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    struct rlimit parent_after;
    getrlimit(RLIMIT_NOFILE, &parent_after);
    check("child limit changes do not modify its parent",
          parent_after.rlim_cur == 9);
    setrlimit(RLIMIT_NOFILE, &original);

    child = fork();
    if (child == 0) {
        struct rlimit lower = { .rlim_cur = 16, .rlim_max = 16 };
        struct rlimit raise = { .rlim_cur = 32, .rlim_max = 32 };
        if (setuid(1000) != 0 || setrlimit(RLIMIT_NOFILE, &lower) != 0)
            _exit(94);
        errno = 0;
        _exit(setrlimit(RLIMIT_NOFILE, &raise) == -1 && errno == EPERM ? 0 : 95);
    }
    check("non-root process cannot raise its hard limit",
          waitpid(child, &status, 0) == child
          && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_usage_accounting(void) {
    struct rusage before, after;
    getrusage(RUSAGE_SELF, &before);
    burn_cpu(150);
    getrusage(RUSAGE_SELF, &after);
    check("busy userspace work increases process CPU usage",
          usage_us(&after) > usage_us(&before) + 20000);

    before = after;
    struct timespec delay = { .tv_nsec = 200000000L };
    nanosleep(&delay, NULL);
    getrusage(RUSAGE_SELF, &after);
    check("sleeping time is not charged as process CPU time",
          usage_us(&after) - usage_us(&before) < 100000);

    before = after;
    pthread_t worker;
    int created = pthread_create(&worker, NULL, thread_burn, (void *)(uintptr_t)120);
    int joined = created ? created : pthread_join(worker, NULL);
    getrusage(RUSAGE_SELF, &after);
    check("worker-thread CPU time contributes to process usage",
          created == 0 && joined == 0
          && usage_us(&after) > usage_us(&before) + 15000);

    struct rusage children_before, children_unreaped, child_usage, children_after;
    getrusage(RUSAGE_CHILDREN, &children_before);
    pid_t child = fork();
    if (child == 0) {
        burn_cpu(120);
        _exit(17);
    }
    struct timespec child_delay = { .tv_nsec = 200000000L };
    nanosleep(&child_delay, NULL);
    getrusage(RUSAGE_CHILDREN, &children_unreaped);
    check("an exited child is not accumulated before it is reaped",
          usage_us(&children_unreaped) == usage_us(&children_before));
    int status = 0;
    memset(&child_usage, 0, sizeof(child_usage));
    pid_t waited = wait4(child, &status, 0, &child_usage);
    getrusage(RUSAGE_CHILDREN, &children_after);
    check("wait4 returns the selected child's CPU usage",
          waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 17
          && usage_us(&child_usage) > 15000);
    check("reaped child usage accumulates in RUSAGE_CHILDREN",
          usage_us(&children_after) >= usage_us(&children_before)
                                      + usage_us(&child_usage));
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--exec-limit"))
        return exec_limit_mode(argv[2]);

    puts("[resource_test] resource limits and accounting");
    test_limit_reporting();
    test_descriptor_enforcement();
    test_inheritance_and_permissions(argv[0]);
    test_usage_accounting();
    printf("[resource_test] === %d failure(s) ===\n", failures);
    return failures ? 1 : 0;
}
