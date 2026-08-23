#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

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

    printf("TTY_TEST_PASS: %ux%u termios round-trip\n",
           (unsigned)size.ws_col, (unsigned)size.ws_row);
    return 0;
}
