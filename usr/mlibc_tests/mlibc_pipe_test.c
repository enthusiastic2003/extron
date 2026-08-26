#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int failures;

static void check(const char *what, int ok) {
    printf("[pipe_test] %-47s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void test_basic_pipe_and_dup(void) {
    int fds[2] = {-1, -1};
    check("pipe() creates two descriptors", pipe(fds) == 0);
    if (fds[0] < 0 || fds[1] < 0)
        return;

    check("write() sends bytes into a pipe", write(fds[1], "hello", 5) == 5);
    struct pollfd readable = { .fd = fds[0], .events = POLLIN };
    check("poll() reports buffered pipe data",
          poll(&readable, 1, 0) == 1 && (readable.revents & POLLIN));

    char buffer[8] = {0};
    check("read() receives the pipe bytes",
          read(fds[0], buffer, 5) == 5 && memcmp(buffer, "hello", 5) == 0);

    int duplicate = dup(fds[1]);
    check("dup() duplicates a pipe endpoint", duplicate >= 0);
    close(fds[1]);
    check("duplicate remains usable after original close",
          duplicate >= 0 && write(duplicate, "!", 1) == 1);
    check("reader receives data from the duplicate",
          read(fds[0], buffer, 1) == 1 && buffer[0] == '!');
    close(duplicate);
    check("read() returns EOF after the final writer closes",
          read(fds[0], buffer, 1) == 0);
    close(fds[0]);
}

static void test_blocking_and_fork(void) {
    int fds[2] = {-1, -1};
    int created = pipe(fds) == 0;
    check("second pipe() succeeds", created);
    if (!created)
        return;

    pid_t child = fork();
    check("fork() creates a streaming writer", child >= 0);
    if (child == 0) {
        close(fds[0]);
        static char payload[8192];
        for (size_t i = 0; i < sizeof(payload); i++)
            payload[i] = (char)('a' + i % 26);
        _exit(write(fds[1], payload, sizeof(payload)) == (ssize_t)sizeof(payload)
              ? 0 : 1);
    }
    if (child < 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }

    close(fds[1]);
    char chunk[257];
    size_t total = 0;
    int contents_ok = 1;
    ssize_t amount;
    while ((amount = read(fds[0], chunk, sizeof(chunk))) > 0) {
        for (ssize_t i = 0; i < amount; i++)
            if (chunk[i] != (char)('a' + (total + (size_t)i) % 26))
                contents_ok = 0;
        total += (size_t)amount;
    }
    close(fds[0]);
    int status = -1;
    check("pipe blocks/wakes across its 4 KiB capacity",
          amount == 0 && total == 8192 && contents_ok);
    check("writer exits after the reader drains the pipe",
          wait(&status) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_redirection_and_fcntl(void) {
    int saved = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 10);
    check("F_DUPFD_CLOEXEC honors the minimum descriptor", saved >= 10);
    check("F_GETFD reports FD_CLOEXEC",
          saved >= 0 && (fcntl(saved, F_GETFD) & FD_CLOEXEC));

    int output = open("pipe-redirection.txt", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    check("open() creates a redirection target", output >= 0);
    int redirected = output >= 0 && dup2(output, STDOUT_FILENO) == STDOUT_FILENO;
    if (output >= 0)
        close(output);
    int wrote = redirected && write(STDOUT_FILENO, "redirected\n", 11) == 11;
    int restored = saved >= 0 && dup2(saved, STDOUT_FILENO) == STDOUT_FILENO;
    if (saved >= 0)
        close(saved);
    check("dup2() redirects and restores stdout", redirected && wrote && restored);

    char buffer[16] = {0};
    int input = open("pipe-redirection.txt", O_RDONLY);
    check("redirection target contains stdout data",
          input >= 0 && read(input, buffer, sizeof(buffer)) == 11
          && memcmp(buffer, "redirected\n", 11) == 0);
    if (input >= 0)
        close(input);
}

static void test_broken_pipe(void) {
    int fds[2] = {-1, -1};
    int created = pipe(fds) == 0;
    check("broken-pipe setup succeeds", created);
    if (!created)
        return;

    close(fds[0]);
    signal(SIGPIPE, SIG_IGN);
    errno = 0;
    check("ignored SIGPIPE leaves write() reporting EPIPE",
          write(fds[1], "x", 1) == -1 && errno == EPIPE);
    signal(SIGPIPE, SIG_DFL);
    close(fds[1]);

    check("create pipe for default SIGPIPE action", pipe(fds) == 0);
    pid_t child = fork();
    if (child == 0) {
        close(fds[0]);
        write(fds[1], "x", 1);
        _exit(99);
    }
    close(fds[0]);
    close(fds[1]);
    int status = 0;
    check("default SIGPIPE terminates the writer",
          child > 0 && waitpid(child, &status, 0) == child
          && WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE);
}

int main(void) {
    puts("[pipe_test] === pipe / dup / fcntl / redirection ===");
    test_basic_pipe_and_dup();
    test_blocking_and_fork();
    test_redirection_and_fcntl();
    test_broken_pipe();
    printf("[pipe_test] === %d failure(s) ===\n", failures);
    return failures != 0;
}
