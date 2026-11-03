/* readv/writev, ftruncate, fsync, getrandom, sched_yield, clock_getres,
 * times, madvise, waitid. */
#include "arch_compat.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <time.h>
#include <signal.h>
#include <sys/uio.h>
#include <sys/times.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define SYS_GETRANDOM 89
static long getrandom(void *buf, size_t len, unsigned flags) {
    long r = extron_raw_syscall(SYS_GETRANDOM, (long)buf, (long)len, (long)flags);
    if (r < 0) { errno = (int)-r; return -1; }
    return r;
}

static int failures;
static void check(const char *what, int ok) {
    printf("[batch_test] %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    const char *path = "/tmp/batch_test.dat";
    char buf[64];
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);

    /* ---- writev / readv ---- */
    struct iovec w[3] = { {"hello ", 6}, {"", 0}, {"world", 5} };
    check("writev 3 iovecs", writev(fd, w, 3) == 11);
    lseek(fd, 0, SEEK_SET);
    char a[4] = {0}, b[16] = {0};
    struct iovec r[2] = { {a, 3}, {b, 16} };
    ssize_t n = readv(fd, r, 2);
    check("readv total", n == 11);
    check("readv scattered data", !memcmp(a, "hel", 3) && !memcmp(b, "lo world", 8));

    /* ---- ftruncate ---- */
    check("ftruncate shrink to 5", ftruncate(fd, 5) == 0);
    struct stat st;
    fstat(fd, &st);
    check("size is 5", st.st_size == 5);
    memset(buf, 'x', sizeof buf);
    check("data kept", pread(fd, buf, 10, 0) == 5 && !memcmp(buf, "hello", 5));
    check("ftruncate grow to 12", ftruncate(fd, 12) == 0);
    memset(buf, 'x', sizeof buf);
    check("grown region is zeros",
          pread(fd, buf, 12, 0) == 12 && !memcmp(buf, "hello\0\0\0\0\0\0\0", 12));
    /* multi-block shrink + stale-data check */
    char big[9000]; memset(big, 'B', sizeof big);
    check("write 9000 bytes", pwrite(fd, big, sizeof big, 0) == 9000);
    check("shrink into middle of block 1", ftruncate(fd, 1500) == 0);
    check("grow back to 9000", ftruncate(fd, 9000) == 0);
    memset(big, 'x', sizeof big);
    check("old tail does not reappear",
          pread(fd, big, 9000, 0) == 9000
          && big[1499] == 'B' && big[1500] == 0 && big[8999] == 0);
    check("ftruncate to 0", ftruncate(fd, 0) == 0);
    fstat(fd, &st);
    check("size is 0", st.st_size == 0);
    errno = 0;
    check("negative length -> EINVAL", ftruncate(fd, -1) == -1 && errno == EINVAL);
    errno = 0;
    check("bad fd -> EBADF", ftruncate(999, 0) == -1 && errno == EBADF);
    close(fd);
    fd = open(path, O_RDONLY);
    errno = 0;
    check("read-only fd -> EINVAL", ftruncate(fd, 0) == -1 && errno == EINVAL);
    close(fd);

    /* ---- fsync ---- */
    fd = open(path, O_RDWR);
    check("fsync", fsync(fd) == 0);
    check("fdatasync", fdatasync(fd) == 0);
    errno = 0;
    check("fsync bad fd -> EBADF", fsync(999) == -1 && errno == EBADF);
    close(fd);
    unlink(path);

    /* ---- getrandom ---- */
    unsigned char r1[64] = {0}, r2[64] = {0}, zero[64] = {0};
    check("getrandom 64", getrandom(r1, 64, 0) == 64);
    check("getrandom again", getrandom(r2, 64, 0) == 64);
    unsigned char ge[32] = {0};
    check("getentropy 32", getentropy(ge, 32) == 0 && memcmp(ge, zero, 32) != 0);
    check("not all zero", memcmp(r1, zero, 64) != 0);
    check("two draws differ", memcmp(r1, r2, 64) != 0);
    unsigned char big2[1000]; 
    check("getrandom 1000 (>256)", getrandom(big2, 1000, 0) == 1000);
    errno = 0;
    check("bad buffer -> EFAULT", getrandom((void *)8, 16, 0) == -1 && errno == EFAULT);

    /* ---- sched_yield ---- */
    check("sched_yield", sched_yield() == 0);

    /* ---- clock_getres ---- */
    struct timespec ts = {99, 99};
    check("clock_getres(MONOTONIC)",
          clock_getres(CLOCK_MONOTONIC, &ts) == 0 && ts.tv_sec == 0 && ts.tv_nsec > 0);

    /* ---- times ---- */
    struct tms t;
    clock_t c1 = times(&t);
    check("times returns a tick count", c1 != (clock_t)-1);
    usleep(20000);
    clock_t c2 = times(&t);
    check("times advances", c2 > c1);

    /* ---- madvise ---- */
    void *m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("posix_madvise", m != MAP_FAILED && posix_madvise(m, 4096, POSIX_MADV_NORMAL) == 0);

    /* ---- waitid ---- */
    pid_t c = fork();
    if (c == 0) _exit(7);
    siginfo_t si;
    memset(&si, 0, sizeof si);
    check("waitid(P_PID, WEXITED)", waitid(P_PID, c, &si, WEXITED) == 0);
    check("si_pid / si_status / si_code",
          si.si_pid == c && si.si_status == 7 && si.si_code == CLD_EXITED);
    c = fork();
    if (c == 0) { raise(SIGKILL); _exit(0); }
    check("waitid(P_ALL) killed child",
          waitid(P_ALL, 0, &si, WEXITED) == 0 && si.si_pid == c
          && si.si_code == CLD_KILLED && si.si_status == SIGKILL);
    memset(&si, 0, sizeof si);
    errno = 0;
    check("waitid no children -> ECHILD",
          waitid(P_ALL, 0, &si, WEXITED | WNOHANG) == -1 && errno == ECHILD);

    printf("[batch_test] %s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
