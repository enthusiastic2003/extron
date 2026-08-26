#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int failures;
static volatile sig_atomic_t usr1_seen;

static void check(const char *what, int ok) {
    printf("[poll_test] %-54s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static long elapsed_ms(struct timespec begin, struct timespec end) {
    return (end.tv_sec - begin.tv_sec) * 1000
         + (end.tv_nsec - begin.tv_nsec) / 1000000;
}

static void delayed_write(int fd, char byte, long milliseconds) {
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = milliseconds % 1000 * 1000000L,
    };
    nanosleep(&delay, NULL);
    _exit(write(fd, &byte, 1) == 1 ? 0 : 1);
}

static void test_timeout_and_pipe_wait(void) {
    struct timespec begin, end;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    int result = poll(NULL, 0, 120);
    clock_gettime(CLOCK_MONOTONIC, &end);
    long elapsed = elapsed_ms(begin, end);
    check("poll with no descriptors observes its timeout",
          result == 0 && elapsed >= 80 && elapsed < 1000);

    int first[2], second[2];
    check("create two pipes for multi-descriptor poll",
          pipe(first) == 0 && pipe(second) == 0);
    pid_t child = fork();
    if (child == 0) {
        close(first[0]); close(first[1]); close(second[0]);
        delayed_write(second[1], 'P', 100);
    }
    close(second[1]);
    struct pollfd fds[2] = {
        { .fd = first[0], .events = POLLIN },
        { .fd = second[0], .events = POLLIN },
    };
    result = poll(fds, 2, 1000);
    char byte = 0;
    check("poll wakes for whichever pipe becomes readable",
          result == 1 && fds[0].revents == 0
          && (fds[1].revents & POLLIN)
          && read(second[0], &byte, 1) == 1 && byte == 'P');
    int status;
    check("delayed pipe writer completed", waitpid(child, &status, 0) == child
          && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(first[0]); close(first[1]); close(second[0]);
}

static void test_nonblocking_and_select(void) {
    int fds[2];
    check("pipe2 accepts O_NONBLOCK and O_CLOEXEC",
          pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);
    char byte;
    errno = 0;
    check("empty nonblocking pipe read reports EAGAIN",
          read(fds[0], &byte, 1) == -1 && errno == EAGAIN);
    check("pipe2 installs O_NONBLOCK on both descriptions",
          (fcntl(fds[0], F_GETFL) & O_NONBLOCK)
          && (fcntl(fds[1], F_GETFL) & O_NONBLOCK));
    int duplicate = dup(fds[0]);
    check("duplicated descriptor shares O_NONBLOCK status",
          duplicate >= 0 && (fcntl(duplicate, F_GETFL) & O_NONBLOCK));
    close(duplicate); close(fds[0]); close(fds[1]);

    check("create a pipe for select", pipe(fds) == 0);
    pid_t child = fork();
    if (child == 0) {
        close(fds[0]);
        delayed_write(fds[1], 'S', 100);
    }
    close(fds[1]);
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(fds[0], &readable);
    struct timeval timeout = { .tv_sec = 1 };
    int result = select(fds[0] + 1, &readable, NULL, NULL, &timeout);
    check("select wakes for readable pipe data",
          result == 1 && FD_ISSET(fds[0], &readable)
          && read(fds[0], &byte, 1) == 1 && byte == 'S');
    int status;
    waitpid(child, &status, 0);
    close(fds[0]);
}

static void usr1_handler(int signo) {
    (void)signo;
    usr1_seen = 1;
}

static void test_pselect_mask(void) {
    struct sigaction action = {0};
    action.sa_handler = usr1_handler;
    sigemptyset(&action.sa_mask);
    check("install SIGUSR1 handler for pselect", sigaction(SIGUSR1, &action, NULL) == 0);

    sigset_t blocked, empty, observed;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    sigemptyset(&empty);
    check("block SIGUSR1 before pselect", sigprocmask(SIG_BLOCK, &blocked, NULL) == 0);

    pid_t child = fork();
    if (child == 0) {
        struct timespec delay = { .tv_nsec = 100000000L };
        nanosleep(&delay, NULL);
        _exit(kill(getppid(), SIGUSR1) == 0 ? 0 : 1);
    }
    struct timespec timeout = { .tv_sec = 1 };
    errno = 0;
    int result = pselect(0, NULL, NULL, NULL, &timeout, &empty);
    check("pselect atomically unmasks and receives SIGUSR1",
          result == -1 && errno == EINTR && usr1_seen);
    sigprocmask(SIG_SETMASK, NULL, &observed);
    check("pselect restores the caller's original signal mask",
          sigismember(&observed, SIGUSR1) == 1);
    sigprocmask(SIG_UNBLOCK, &blocked, NULL);
    int status;
    waitpid(child, &status, 0);
}

static void test_pty_nonblocking(void) {
    int master = posix_openpt(O_RDWR | O_NONBLOCK);
    check("open nonblocking PTY master", master >= 0);
    if (master < 0) return;
    check("unlock PTY master", grantpt(master) == 0 && unlockpt(master) == 0);
    char *name = ptsname(master);
    int slave = name ? open(name, O_RDWR | O_NONBLOCK) : -1;
    check("open PTY slave", slave >= 0);
    char byte;
    errno = 0;
    check("empty nonblocking PTY master read reports EAGAIN",
          read(master, &byte, 1) == -1 && errno == EAGAIN);

    errno = 0;
    check("empty nonblocking PTY slave read reports EAGAIN",
          slave >= 0 && read(slave, &byte, 1) == -1 && errno == EAGAIN);
    struct pollfd slave_readable = { .fd = slave, .events = POLLIN };
    check("canonical PTY is not readable before line completion",
          write(master, "x", 1) == 1
          && poll(&slave_readable, 1, 0) == 0);
    check("newline makes the canonical PTY slave readable",
          write(master, "\n", 1) == 1
          && poll(&slave_readable, 1, 0) == 1
          && (slave_readable.revents & POLLIN));
    char line[2];
    check("canonical nonblocking read returns the completed line",
          read(slave, line, sizeof(line)) == 2
          && line[0] == 'x' && line[1] == '\n');
    char echoed[2];
    check("PTY canonical echo is returned through the master",
          read(master, echoed, sizeof(echoed)) == 2
          && echoed[0] == 'x' && echoed[1] == '\n');

    check("slave output reaches and wakes the PTY master",
          slave >= 0 && write(slave, "T", 1) == 1);
    struct pollfd readable = { .fd = master, .events = POLLIN };
    check("poll reports buffered PTY master output",
          poll(&readable, 1, 0) == 1 && (readable.revents & POLLIN)
          && read(master, &byte, 1) == 1 && byte == 'T');

    struct termios raw;
    int raw_ok = tcgetattr(slave, &raw) == 0;
    if (raw_ok) {
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        raw_ok = tcsetattr(slave, TCSANOW, &raw) == 0;
    }
    check("put PTY slave into raw mode for backpressure", raw_ok);
    char block[256];
    memset(block, 'q', sizeof(block));
    size_t filled = 0;
    ssize_t amount;
    while ((amount = write(master, block, sizeof(block))) > 0)
        filled += (size_t)amount;
    check("nonblocking PTY master stops with EAGAIN when full",
          filled > 0 && amount == -1 && errno == EAGAIN);
    struct pollfd writable = { .fd = master, .events = POLLOUT };
    check("full PTY input buffer is not reported writable",
          poll(&writable, 1, 0) == 0);
    check("reading the slave frees one input position",
          read(slave, &byte, 1) == 1 && byte == 'q');
    check("PTY master becomes writable after slave consumes data",
          poll(&writable, 1, 0) == 1 && (writable.revents & POLLOUT));
    if (slave >= 0) close(slave);
    close(master);
}

int main(void) {
    puts("[poll_test] === readiness / nonblocking / pselect ===");
    test_timeout_and_pipe_wait();
    test_nonblocking_and_select();
    test_pselect_mask();
    test_pty_nonblocking();
    printf("[poll_test] === %d failure(s) ===\n", failures);
    return failures != 0;
}
