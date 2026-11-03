/*
 * pread(2): reads at an explicit offset and must NOT move the descriptor's
 * position. Also checks the failure modes: ESPIPE on a pipe, EINVAL for a
 * negative offset, EBADF for a write-only descriptor, a short read at EOF.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

static int failures;
static void check(const char *what, int ok) {
    printf("[pread_test] %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    const char *path = "/tmp/pread_test.dat";
    const char data[] = "0123456789abcdefghij";   /* 20 bytes + NUL */
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    check("open", fd >= 0);
    check("write", write(fd, data, 20) == 20);

    char buf[32];
    memset(buf, 0, sizeof buf);
    check("pread 5 bytes at offset 10",
          pread(fd, buf, 5, 10) == 5 && !memcmp(buf, "abcde", 5));
    check("pread did not move the position",
          lseek(fd, 0, SEEK_CUR) == 20);

    check("seek to 3", lseek(fd, 3, SEEK_SET) == 3);
    check("pread at 0 returns the start",
          pread(fd, buf, 4, 0) == 4 && !memcmp(buf, "0123", 4));
    check("position still 3 after pread", lseek(fd, 0, SEEK_CUR) == 3);
    check("plain read still uses position 3",
          read(fd, buf, 3) == 3 && !memcmp(buf, "345", 3));
    check("position advanced by read only", lseek(fd, 0, SEEK_CUR) == 6);

    check("pread straddling EOF is short",
          pread(fd, buf, 10, 15) == 5 && !memcmp(buf, "fghij", 5));
    check("pread at EOF returns 0", pread(fd, buf, 4, 20) == 0);
    check("pread past EOF returns 0", pread(fd, buf, 4, 1000) == 0);
    check("pread count 0 returns 0", pread(fd, buf, 0, 5) == 0);

    errno = 0;
    check("negative offset -> EINVAL",
          pread(fd, buf, 4, -1) == -1 && errno == EINVAL);
    errno = 0;
    check("bad fd -> EBADF", pread(999, buf, 4, 0) == -1 && errno == EBADF);
    errno = 0;
    check("bad buffer -> EFAULT",
          pread(fd, (void *)8, 4, 0) == -1 && errno == EFAULT);
    close(fd);

    fd = open(path, O_WRONLY);
    errno = 0;
    check("write-only fd -> EBADF",
          pread(fd, buf, 4, 0) == -1 && errno == EBADF);
    close(fd);

    int p[2];
    check("pipe", pipe(p) == 0);
    errno = 0;
    check("pipe -> ESPIPE", pread(p[0], buf, 4, 0) == -1 && errno == ESPIPE);
    close(p[0]); close(p[1]);

    fd = open("/dev/console", O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
        errno = 0;
        check("tty -> ESPIPE",
              pread(fd, buf, 4, 0) == -1 && errno == ESPIPE);
        close(fd);
    }

    unlink(path);
    printf("[pread_test] %s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
