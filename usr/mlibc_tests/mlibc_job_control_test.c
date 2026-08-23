#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t chld_events;
static volatile sig_atomic_t last_child;
static volatile sig_atomic_t last_code;

static void chld_handler(int signo, siginfo_t *info, void *context) {
    (void)context;
    if (signo == SIGCHLD && info) {
        chld_events++;
        last_child = info->si_pid;
        last_code = info->si_code;
    }
}

static int report(const char *name, int okay) {
    printf("[job_test] %-55s %s\n", name, okay ? "PASS" : "FAIL");
    return okay ? 0 : 1;
}

int main(void) {
    int failures = 0;
    struct sigaction action = {0};
    action.sa_sigaction = chld_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    failures += report("install SIGCHLD SA_SIGINFO handler",
                       sigaction(SIGCHLD, &action, NULL) == 0);

    pid_t own_group = getpgrp();
    failures += report("foreground TTY belongs to calling process group",
                       own_group > 0 && tcgetpgrp(0) == own_group);

    pid_t child = fork();
    if (!child) {
        setpgid(0, 0);
        for (;;) __asm__ volatile ("");
    }
    int grouped = setpgid(child, child) == 0 || getpgid(child) == child;
    failures += report("child can become leader of a separate process group",
                       grouped && getpgid(child) == child);

    int status = 0;
    failures += report("waitpid(WNOHANG) leaves a running child alone",
                       waitpid(child, &status, WNOHANG) == 0);
    failures += report("foreground process group can be changed and restored",
                       tcsetpgrp(0, child) == 0 && tcgetpgrp(0) == child
                       && tcsetpgrp(0, own_group) == 0);

    kill(-child, SIGSTOP);
    pid_t waited = waitpid(child, &status, WUNTRACED);
    failures += report("SIGSTOP suspends instead of terminating",
                       waited == child && WIFSTOPPED(status)
                       && WSTOPSIG(status) == SIGSTOP);

    kill(-child, SIGCONT);
    waited = waitpid(child, &status, WCONTINUED);
    failures += report("SIGCONT resumes and reports the transition",
                       waited == child && WIFCONTINUED(status));

    kill(-child, SIGTERM);
    waited = waitpid(child, &status, 0);
    if (waited != child || !WIFSIGNALED(status)
            || WTERMSIG(status) != SIGTERM)
        printf("[job_test] SIGTERM detail: waitpid=%d status=0x%x "
               "signaled=%d termsig=%d\n", (int)waited, status,
               WIFSIGNALED(status), WTERMSIG(status));
    failures += report("continued process remains normally signalable",
                       waited == child && WIFSIGNALED(status)
                       && WTERMSIG(status) == SIGTERM);
    failures += report("SIGCHLD includes the transitioning child identity",
                       chld_events >= 3 && last_child == child && last_code != 0);

    failures += report("SIGSTOP cannot be caught",
                       signal(SIGSTOP, SIG_IGN) == SIG_ERR);

    int ready_pipe[2] = {-1, -1};
    int data_pipe[2] = {-1, -1};
    int pipes_ok = pipe(ready_pipe) == 0 && pipe(data_pipe) == 0;
    failures += report("blocked-wait preservation pipes are created", pipes_ok);
    if (pipes_ok) {
        pid_t waiter = fork();
        if (!waiter) {
            close(ready_pipe[0]);
            close(data_pipe[1]);
            char byte = 'r';
            if (write(ready_pipe[1], &byte, 1) != 1)
                _exit(1);
            _exit(read(data_pipe[0], &byte, 1) == 1 && byte == 'x' ? 42 : 2);
        }
        close(ready_pipe[1]);
        close(data_pipe[0]);
        char byte = 0;
        int child_blocked = waiter > 0 && read(ready_pipe[0], &byte, 1) == 1;
        kill(waiter, SIGSTOP);
        int stop_status = 0;
        int stopped = waitpid(waiter, &stop_status, WUNTRACED) == waiter
            && WIFSTOPPED(stop_status);
        int event_while_stopped = write(data_pipe[1], "x", 1) == 1;
        kill(waiter, SIGCONT);
        int continue_status = 0;
        int continued = waitpid(waiter, &continue_status, WCONTINUED) == waiter
            && WIFCONTINUED(continue_status);
        int exit_status = 0;
        int completed = waitpid(waiter, &exit_status, 0) == waiter
            && WIFEXITED(exit_status) && WEXITSTATUS(exit_status) == 42;
        failures += report("stopped pipe read retains its blocked operation",
                           child_blocked && stopped && event_while_stopped
                           && continued && completed);
        close(ready_pipe[0]);
        close(data_pipe[1]);
    }

    int timer_ready[2] = {-1, -1};
    int timer_pipe_ok = pipe(timer_ready) == 0;
    failures += report("stopped-timer preservation pipe is created",
                       timer_pipe_ok);
    if (timer_pipe_ok) {
        pid_t timer_child = fork();
        if (!timer_child) {
            close(timer_ready[0]);
            char byte = 'r';
            if (write(timer_ready[1], &byte, 1) != 1)
                _exit(1);
            _exit(sleep(1) == 0 ? 42 : 2);
        }
        close(timer_ready[1]);
        char byte = 0;
        int child_sleeping = timer_child > 0
            && read(timer_ready[0], &byte, 1) == 1;
        kill(timer_child, SIGSTOP);
        int stop_status = 0;
        int stopped = waitpid(timer_child, &stop_status, WUNTRACED)
            == timer_child && WIFSTOPPED(stop_status);
        sleep(2); /* let the child's original one-second deadline expire */
        int still_stopped = waitpid(timer_child, NULL, WNOHANG) == 0;
        kill(timer_child, SIGCONT);
        int continue_status = 0;
        int continued = waitpid(timer_child, &continue_status, WCONTINUED)
            == timer_child && WIFCONTINUED(continue_status);
        int exit_status = 0;
        int completed = waitpid(timer_child, &exit_status, 0) == timer_child
            && WIFEXITED(exit_status) && WEXITSTATUS(exit_status) == 42;
        failures += report("stopped sleep retains its original deadline",
                           child_sleeping && stopped && still_stopped
                           && continued && completed);
        close(timer_ready[0]);
    }
    printf("[job_test] === %d failure(s) ===\n", failures);
    return failures ? 1 : 0;
}
