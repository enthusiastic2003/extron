#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t usr1_count;
static volatile sig_atomic_t term_seen;
static volatile sig_atomic_t info_seen;

static void usr1_handler(int signo) {
    if (signo == SIGUSR1)
        usr1_count++;
}

static void term_handler(int signo) {
    if (signo == SIGTERM)
        term_seen = 1;
}

static void info_handler(int signo, siginfo_t *info, void *context) {
    ucontext_t *ucontext = context;
    if (signo == SIGUSR2 && info && ucontext
            && info->si_signo == SIGUSR2
            && info->si_code == SI_USER
            && info->si_pid == getpid()
            && ucontext->uc_mcontext.pc != 0)
        info_seen = 1;
}

static void segv_handler(int signo) {
    _Exit(signo == SIGSEGV ? 42 : 43);
}

__attribute__((noinline, noreturn))
static void cause_data_abort(void) {
    uintptr_t address = 0;
    __asm__ volatile ("ldr x9, [%0]" :: "r"(address) : "x9", "memory");
    __builtin_unreachable();
}

static int pass(const char *name, int okay) {
    printf("[signal_test] %-52s %s\n", name, okay ? "PASS" : "FAIL");
    return okay ? 0 : 1;
}

static int signal_preserves_simd(void) {
    uint64_t input[2] __attribute__((aligned(16))) = {
        0x1122334455667788ULL, 0x99aabbccddeeff00ULL
    };
    uint64_t output[2] __attribute__((aligned(16))) = {0, 0};
    register long x0 __asm__("x0") = getpid();
    __asm__ volatile (
        "mov x8, #36\n" /* SYS_KILL */
        "mov x1, #10\n" /* SIGUSR1 */
        "ldr q0, [%1]\n"
        "svc #0\n"
        "str q0, [%2]\n"
        : "+r"(x0)
        : "r"(input), "r"(output)
        : "x1", "x8", "q0", "memory", "cc");
    return output[0] == input[0] && output[1] == input[1];
}

int main(void) {
    int failures = 0;
    failures += pass("signal()/raise() handler and sigreturn",
                     signal(SIGUSR1, usr1_handler) != SIG_ERR
                     && raise(SIGUSR1) == 0 && usr1_count == 1);
    failures += pass("signal return restores interrupted SIMD registers",
                     signal_preserves_simd() && usr1_count == 2);

    struct sigaction info_action = {0};
    info_action.sa_sigaction = info_handler;
    info_action.sa_flags = SA_SIGINFO;
    sigemptyset(&info_action.sa_mask);
    failures += pass("SA_SIGINFO receives siginfo_t and ucontext_t",
                     sigaction(SIGUSR2, &info_action, NULL) == 0
                     && raise(SIGUSR2) == 0 && info_seen == 1);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    int blocked = sigprocmask(SIG_BLOCK, &set, NULL) == 0;
    int queued = raise(SIGUSR1) == 0 && usr1_count == 2;
    int unblocked = sigprocmask(SIG_UNBLOCK, &set, NULL) == 0;
    failures += pass("blocked signal stays pending then delivers on unblock",
                     blocked && queued && unblocked && usr1_count == 3);

    int ready[2];
    if (pipe(ready) != 0)
        return 1;
    pid_t child = fork();
    if (!child) {
        close(ready[0]);
        signal(SIGTERM, term_handler);
        write(ready[1], "r", 1);
        while (!term_seen) __asm__ volatile ("");
        _Exit(23);
    }
    close(ready[1]);
    char byte;
    read(ready[0], &byte, 1);
    close(ready[0]);
    kill(child, SIGTERM);
    int status = 0;
    wait(&status);
    failures += pass("kill() delivers SIGTERM handler in another process",
                     WIFEXITED(status) && WEXITSTATUS(status) == 23);

    child = fork();
    if (!child) {
        signal(SIGSEGV, segv_handler);
        cause_data_abort();
    }
    wait(&status);
    failures += pass("synchronous SIGSEGV reaches installed handler",
                     WIFEXITED(status) && WEXITSTATUS(status) == 42);

    child = fork();
    if (!child)
        for (;;) __asm__ volatile ("");
    kill(child, SIGTERM);
    wait(&status);
    failures += pass("default SIGTERM action terminates child",
                     WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);

    printf("[signal_test] === %d failure(s) ===\n", failures);
    return failures ? 1 : 0;
}
