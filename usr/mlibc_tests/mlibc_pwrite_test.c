/* pwrite(2): writes at an explicit offset without moving the position;
 * under O_APPEND it appends regardless of the offset (Linux semantics). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

static int failures;
static void check(const char *what, int ok) {
    printf("[pwrite_test] %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    const char *path = "/tmp/pwrite_test.dat";
    char buf[64];
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    check("open", fd >= 0);
    check("write base", write(fd, "AAAAAAAAAA", 10) == 10);   /* pos 10 */

    check("pwrite 3 bytes at 2", pwrite(fd, "xyz", 3, 2) == 3);
    check("position unchanged by pwrite", lseek(fd, 0, SEEK_CUR) == 10);
    memset(buf, 0, sizeof buf);
    check("contents reflect pwrite",
          pread(fd, buf, 10, 0) == 10 && !memcmp(buf, "AAxyzAAAAA", 10));

    check("pwrite past EOF extends", pwrite(fd, "Z", 1, 14) == 1);
    check("size is now 15", lseek(fd, 0, SEEK_END) == 15);
    check("hole reads as zeros",
          pread(fd, buf, 5, 10) == 5 && !memcmp(buf, "\0\0\0\0Z", 5));

    check("seek to 4", lseek(fd, 4, SEEK_SET) == 4);
    check("plain write uses position", write(fd, "QQ", 2) == 2);
    check("position advanced by write only", lseek(fd, 0, SEEK_CUR) == 6);
    check("pread sees plain write",
          pread(fd, buf, 6, 0) == 6 && !memcmp(buf, "AAxyQQ", 6));

    errno = 0;
    check("negative offset -> EINVAL",
          pwrite(fd, "a", 1, -1) == -1 && errno == EINVAL);
    errno = 0;
    check("bad fd -> EBADF", pwrite(999, "a", 1, 0) == -1 && errno == EBADF);
    errno = 0;
    check("bad buffer -> EFAULT",
          pwrite(fd, (void *)8, 4, 0) == -1 && errno == EFAULT);
    close(fd);

    fd = open(path, O_RDONLY);
    errno = 0;
    check("read-only fd -> EBADF",
          pwrite(fd, "a", 1, 0) == -1 && errno == EBADF);
    close(fd);

    fd = open(path, O_RDWR | O_APPEND);
    off_t size = lseek(fd, 0, SEEK_END);
    check("O_APPEND pwrite appends", pwrite(fd, "TAIL", 4, 0) == 4);
    check("size grew by 4", lseek(fd, 0, SEEK_END) == size + 4);
    check("tail landed at EOF",
          pread(fd, buf, 4, size) == 4 && !memcmp(buf, "TAIL", 4));
    close(fd);

    int p[2];
    check("pipe", pipe(p) == 0);
    errno = 0;
    check("pipe -> ESPIPE", pwrite(p[1], "a", 1, 0) == -1 && errno == ESPIPE);
    close(p[0]); close(p[1]);

    unlink(path);
    printf("[pwrite_test] %s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
