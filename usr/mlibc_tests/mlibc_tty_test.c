#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <fcntl.h>

static int fail(const char *what) {
    printf("TTY_TEST_FAIL: %s: %s\n", what, strerror(errno));
    return 1;
}

int main(void) {
    if (!isatty(STDIN_FILENO))
        return fail("stdin is not a tty");

    struct termios saved;
    if (tcgetattr(STDIN_FILENO, &saved) != 0)
        return fail("tcgetattr");

    struct winsize size;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) != 0)
        return fail("TIOCGWINSZ");
    if (!size.ws_row || !size.ws_col) {
        puts("TTY_TEST_FAIL: zero terminal dimensions");
        return 1;
    }

    struct pollfd writable = {
        .fd = STDOUT_FILENO,
        .events = POLLOUT,
    };
    if (poll(&writable, 1, 0) != 1 || !(writable.revents & POLLOUT)) {
        puts("TTY_TEST_FAIL: stdout did not poll writable");
        return 1;
    }

    struct pollfd invalid = {
        .fd = 99,
        .events = POLLIN,
    };
    if (poll(&invalid, 1, 0) != 1 || !(invalid.revents & POLLNVAL)) {
        puts("TTY_TEST_FAIL: invalid descriptor did not report POLLNVAL");
        return 1;
    }

    /* Discard a possible LF left by a CRLF command terminator, then prove
     * that a zero-timeout readiness check does not invent input. */
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved) != 0)
        return fail("TCSAFLUSH");
    struct pollfd readable = {
        .fd = STDIN_FILENO,
        .events = POLLIN,
    };
    if (poll(&readable, 1, 0) != 0 || readable.revents != 0) {
        puts("TTY_TEST_FAIL: stdin unexpectedly readable after flush");
        return 1;
    }

    int original_flags = fcntl(STDIN_FILENO, F_GETFL);
    if (original_flags < 0
            || fcntl(STDIN_FILENO, F_SETFL, original_flags | O_NONBLOCK) != 0)
        return fail("enable O_NONBLOCK");
    char no_input;
    errno = 0;
    if (read(STDIN_FILENO, &no_input, 1) != -1 || errno != EAGAIN) {
        puts("TTY_TEST_FAIL: empty nonblocking tty did not report EAGAIN");
        fcntl(STDIN_FILENO, F_SETFL, original_flags);
        return 1;
    }
    if (fcntl(STDIN_FILENO, F_SETFL, original_flags) != 0)
        return fail("restore descriptor flags");

    struct termios changed = saved;
    changed.c_lflag ^= ECHO | ICANON;
    changed.c_cc[VMIN] = 1;
    changed.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &changed) != 0)
        return fail("tcsetattr changed");

    struct termios observed;
    if (tcgetattr(STDIN_FILENO, &observed) != 0)
        return fail("tcgetattr changed");
    if ((observed.c_lflag & (ECHO | ICANON))
            != (changed.c_lflag & (ECHO | ICANON))) {
        puts("TTY_TEST_FAIL: termios state did not round-trip");
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        return 1;
    }

    if (tcsetattr(STDIN_FILENO, TCSANOW, &saved) != 0)
        return fail("tcsetattr restore");

    printf("TTY_TEST_PASS: %ux%u termios + poll + nonblock\n",
           (unsigned)size.ws_col, (unsigned)size.ws_row);
    return 0;
}
