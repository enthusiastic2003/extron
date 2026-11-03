/* alarm / setitimer / getitimer (ITIMER_REAL) and sigaltstack + SA_ONSTACK. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/wait.h>

static int failures;
static void check(const char *what, int ok) {
    printf("[timer_test] %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* usleep() returns early when a signal arrives, so sleep against the clock. */
static void sleep_ms(int ms) {
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        long el = (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
        if (el >= ms) return;
        usleep(5000);
    }
}

static volatile sig_atomic_t alarms;
static void on_alarm(int s) { (void)s; alarms++; }

static char altstack[16384];
static volatile char *handler_sp;
static volatile int on_alt;
static void on_usr1(int s) {
    (void)s;
    char marker;
    handler_sp = &marker;
    stack_t cur;
    sigaltstack(NULL, &cur);
    on_alt = (cur.ss_flags & SS_ONSTACK) != 0;
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);

    /* ---- one-shot setitimer ---- */
    struct itimerval it = { {0, 0}, {0, 50000} };       /* 50 ms */
    check("setitimer one-shot", setitimer(ITIMER_REAL, &it, NULL) == 0);
    struct itimerval cur;
    check("getitimer reports remaining time",
          getitimer(ITIMER_REAL, &cur) == 0
          && cur.it_value.tv_sec == 0 && cur.it_value.tv_usec > 0
          && cur.it_value.tv_usec <= 50000);
    sleep_ms(150);
    check("SIGALRM delivered once", alarms == 1);
    getitimer(ITIMER_REAL, &cur);
    check("one-shot disarmed afterwards",
          cur.it_value.tv_sec == 0 && cur.it_value.tv_usec == 0);

    /* ---- periodic ---- */
    alarms = 0;
    struct itimerval per = { {0, 30000}, {0, 30000} };  /* every 30 ms */
    setitimer(ITIMER_REAL, &per, NULL);
    sleep_ms(200);
    struct itimerval off = { {0, 0}, {0, 0} };
    setitimer(ITIMER_REAL, &off, NULL);
    int got = alarms;
    check("periodic timer fired repeatedly (>=3)", got >= 3);
    sleep_ms(100);
    check("cancelled timer stops firing", alarms == got);

    /* ---- alarm() ---- */
    alarms = 0;
    check("alarm(1) returns 0 when none pending", alarm(1) == 0);
    unsigned left = alarm(0);
    check("alarm(0) cancels and returns remaining", left == 1);
    sleep_ms(50);
    check("cancelled alarm did not fire", alarms == 0);
    alarm(1);
    pause();                                            /* woken by SIGALRM */
    check("pause() interrupted by alarm", alarms == 1);

    /* ---- error cases ---- */
    errno = 0;
    check("ITIMER_VIRTUAL unsupported -> EINVAL",
          setitimer(ITIMER_VIRTUAL, &it, NULL) == -1 && errno == EINVAL);
    struct itimerval bad = { {0, 0}, {0, 2000000} };
    errno = 0;
    check("usec out of range -> EINVAL",
          setitimer(ITIMER_REAL, &bad, NULL) == -1 && errno == EINVAL);

    /* ---- timer does not survive fork ---- */
    alarms = 0;
    alarm(1);
    pid_t c = fork();
    if (c == 0) {
        struct itimerval t;
        getitimer(ITIMER_REAL, &t);
        _exit(t.it_value.tv_sec == 0 && t.it_value.tv_usec == 0 ? 0 : 1);
    }
    int st = 0; waitpid(c, &st, 0);
    check("child starts with no timer", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    alarm(0);

    /* ---- sigaltstack ---- */
    stack_t old;
    check("sigaltstack query: initially disabled",
          sigaltstack(NULL, &old) == 0 && (old.ss_flags & SS_DISABLE));
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0 };
    check("sigaltstack install", sigaltstack(&ss, NULL) == 0);
    check("query returns what was installed",
          sigaltstack(NULL, &old) == 0 && old.ss_sp == altstack
          && old.ss_size == sizeof altstack && !(old.ss_flags & SS_DISABLE));

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sa.sa_flags = SA_ONSTACK;
    sigaction(SIGUSR1, &sa, NULL);
    raise(SIGUSR1);
    check("SA_ONSTACK handler ran on the alternate stack",
          handler_sp >= altstack && handler_sp < altstack + sizeof altstack);
    check("SS_ONSTACK reported inside handler", on_alt == 1);

    /* a handler without SA_ONSTACK stays on the normal stack */
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);
    raise(SIGUSR1);
    check("no SA_ONSTACK -> normal stack",
          !(handler_sp >= altstack && handler_sp < altstack + sizeof altstack));

    stack_t tiny = { .ss_sp = altstack, .ss_size = 100, .ss_flags = 0 };
    errno = 0;
    check("too-small stack -> ENOMEM",
          sigaltstack(&tiny, NULL) == -1 && errno == ENOMEM);

    stack_t dis = { .ss_flags = SS_DISABLE };
    check("sigaltstack disable", sigaltstack(&dis, NULL) == 0);
    check("disabled again",
          sigaltstack(NULL, &old) == 0 && (old.ss_flags & SS_DISABLE));

    printf("[timer_test] %s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
