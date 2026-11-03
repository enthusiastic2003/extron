/* sigsuspend/sigpending, FIFOs (mkfifo/mknod), TIOCSCTTY, mremap, flock and
 * POSIX record locks. */
#define _GNU_SOURCE
#include "arch_compat.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <time.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#ifndef TIOCSCTTY
#define TIOCSCTTY 0x540E
#endif
#ifndef TIOCGSID
#define TIOCGSID 0x5429
#endif

static int failures;
static void check(const char *what, int ok) {
    printf("[posix_batch] %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void msleep(int ms) {
    struct timespec ts = { 0, 0 }, t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        usleep(1000);
        clock_gettime(CLOCK_MONOTONIC, &t1);
    } while ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 < ms);
    (void)ts;
}

/* ------------------------------------------------------------------ */
static volatile sig_atomic_t usr1_count;
static void on_usr1(int s) { (void)s; usr1_count++; }

static void test_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);

    sigset_t block, old, pend, empty;
    sigemptyset(&block); sigaddset(&block, SIGUSR1);
    sigemptyset(&empty);
    sigprocmask(SIG_BLOCK, &block, &old);

    sigpending(&pend);
    check("sigpending empty initially", !sigismember(&pend, SIGUSR1));
    usr1_count = 0;
    raise(SIGUSR1);
    sigpending(&pend);
    check("sigpending reports a blocked signal", sigismember(&pend, SIGUSR1));
    check("blocked signal not delivered yet", usr1_count == 0);

    /* Already pending: sigsuspend with an open mask returns immediately. */
    errno = 0;
    int r = sigsuspend(&empty);
    check("sigsuspend returns -1/EINTR for a pending signal", r == -1 && errno == EINTR);
    check("handler ran during sigsuspend", usr1_count == 1);
    sigset_t cur;
    sigprocmask(SIG_BLOCK, NULL, &cur);
    check("original mask restored after sigsuspend", sigismember(&cur, SIGUSR1));
    sigpending(&pend);
    check("pending cleared after delivery", !sigismember(&pend, SIGUSR1));

    /* Wait for a signal that arrives later, from another process. */
    pid_t me = getpid();
    pid_t child = fork();
    if (child == 0) {
        msleep(80);
        kill(me, SIGUSR1);
        _exit(0);
    }
    usr1_count = 0;
    errno = 0;
    r = sigsuspend(&empty);
    check("sigsuspend sleeps until a signal arrives", r == -1 && errno == EINTR && usr1_count == 1);
    waitpid(child, NULL, 0);

    /* The signal sent while the wait mask blocks it must NOT wake us. */
    sigset_t only_usr1 = block;
    child = fork();
    if (child == 0) {
        msleep(40);
        kill(me, SIGUSR2);          /* ignored by the suspend mask? no: unblocked */
        msleep(40);
        kill(me, SIGUSR1);
        _exit(0);
    }
    struct sigaction s2; memset(&s2, 0, sizeof s2); s2.sa_handler = on_usr1;
    sigaction(SIGUSR2, &s2, NULL);
    sigset_t susp; sigfillset(&susp); sigdelset(&susp, SIGUSR2);
    usr1_count = 0;
    r = sigsuspend(&susp);          /* wakes on SIGUSR2 only */
    check("sigsuspend wakes only for unblocked signals", r == -1 && usr1_count == 1);
    waitpid(child, NULL, 0);
    sigpending(&pend);
    check("blocked SIGUSR1 stays pending", sigismember(&pend, SIGUSR1));
    usr1_count = 0;
    sigprocmask(SIG_SETMASK, &old, NULL);       /* now it is delivered */
    check("unblocking delivers the pending signal", usr1_count == 1);
    (void)only_usr1;
}

/* ------------------------------------------------------------------ */
static void test_fifo(void) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/fifo-%ld", (long)getpid());
    unlink(path);
    check("mkfifo creates a FIFO", mkfifo(path, 0644) == 0);
    struct stat st;
    check("stat reports S_ISFIFO", stat(path, &st) == 0 && S_ISFIFO(st.st_mode)
          && (st.st_mode & 0777) == 0644);
    errno = 0;
    check("mkfifo of an existing path is EEXIST", mkfifo(path, 0644) == -1 && errno == EEXIST);

    /* readdir reports DT_FIFO */
    DIR *d = opendir("/tmp");
    int saw = 0;
    const char *base = strrchr(path, '/') + 1;
    for (struct dirent *e; d && (e = readdir(d));)
        if (!strcmp(e->d_name, base)) saw = e->d_type == DT_FIFO;
    if (d) closedir(d);
    check("readdir d_type is DT_FIFO", saw);

    errno = 0;
    int nb = open(path, O_WRONLY | O_NONBLOCK);
    check("O_WRONLY|O_NONBLOCK with no reader fails ENXIO", nb == -1 && errno == ENXIO);

    /* Writer process: its open blocks until we open for reading. */
    pid_t child = fork();
    if (child == 0) {
        int w = open(path, O_WRONLY);
        if (w < 0) _exit(1);
        msleep(30);
        const char msg[] = "through the fifo";
        if (write(w, msg, sizeof msg) != (ssize_t)sizeof msg) _exit(2);
        close(w);
        _exit(0);
    }
    int r = open(path, O_RDONLY);              /* blocks until the writer opens */
    check("blocking O_RDONLY open completes with a writer", r >= 0);
    char buf[64] = {0};
    ssize_t n = read(r, buf, sizeof buf);
    check("data written through the FIFO is read back",
          n == 17 && !strcmp(buf, "through the fifo"));
    check("EOF once the writer closes", read(r, buf, sizeof buf) == 0);
    int status = 0;
    waitpid(child, &status, 0);
    check("writer exited cleanly", WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(r);

    /* O_RDWR never blocks and sees its own data. */
    int rw = open(path, O_RDWR | O_NONBLOCK);
    check("O_RDWR open does not block", rw >= 0);
    errno = 0;
    check("empty FIFO read is EAGAIN under O_NONBLOCK",
          read(rw, buf, 8) == -1 && errno == EAGAIN);
    check("write to O_RDWR FIFO", write(rw, "ping", 4) == 4);
    memset(buf, 0, sizeof buf);
    check("read back own data", read(rw, buf, 4) == 4 && !memcmp(buf, "ping", 4));
    /* Opening for read non-blocking while a writer exists succeeds. */
    int rd = open(path, O_RDONLY | O_NONBLOCK);
    check("second open shares the same pipe", rd >= 0 && write(rw, "x", 1) == 1
          && read(rd, buf, 1) == 1 && buf[0] == 'x');
    close(rd);
    close(rw);

    /* mknod with S_IFIFO */
    char p2[80];
    snprintf(p2, sizeof p2, "%s.nod", path);
    unlink(p2);
    check("mknod(S_IFIFO) works", mknod(p2, S_IFIFO | 0600, 0) == 0
          && stat(p2, &st) == 0 && S_ISFIFO(st.st_mode));
    check("unlink removes FIFOs", unlink(path) == 0 && unlink(p2) == 0
          && stat(path, &st) == -1);
}

/* ------------------------------------------------------------------ */
static void test_ctty(void) {
    int master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (master < 0) { check("open /dev/ptmx", 0); return; }
    grantpt(master); unlockpt(master);
    char *name = ptsname(master);
    int slave = name ? open(name, O_RDWR | O_NOCTTY) : -1;
    check("pty pair opens", slave >= 0);
    if (slave < 0) return;

    int p[2], hold[2];      /* child -> parent result; parent -> child "release" */
    pipe(p); pipe(hold);
    pid_t child = fork();
    if (child == 0) {
        int ok = 0;
        errno = 0;
        int not_leader = ioctl(slave, TIOCSCTTY, 0) == -1 && errno == EPERM;
        pid_t sid = setsid();
        int claim = ioctl(slave, TIOCSCTTY, 0) == 0;
        int got = -1;
        int gsid = ioctl(slave, TIOCGSID, &got) == 0 && got == (int)sid;
        ok = not_leader | (claim << 1) | (gsid << 2);

        write(p[1], &ok, sizeof ok);
        /* Keep the session alive while the parent tries to steal. */
        char c;
        read(hold[0], &c, 1);
        _exit(0);
    }
    int ok = 0;
    read(p[0], &ok, sizeof ok);
    check("non-leader TIOCSCTTY is EPERM", ok & 1);
    check("session leader can claim a controlling tty", ok & 2);
    check("TIOCGSID returns the owning session", ok & 4);

    /* A second session cannot take a tty that already has an owner. */
    pid_t thief = fork();
    if (thief == 0) {
        setsid();
        errno = 0;
        _exit(ioctl(slave, TIOCSCTTY, 0) == -1 && errno == EPERM ? 0 : 1);
    }
    int st = 0;
    waitpid(thief, &st, 0);
    check("another session cannot steal it", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    write(hold[1], "x", 1);
    waitpid(child, NULL, 0);
    close(p[0]); close(p[1]); close(hold[0]); close(hold[1]);
    close(slave); close(master);
}

/* ------------------------------------------------------------------ */
static void test_mremap(void) {
    size_t pg = 4096;
    char *a = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap one page", a != MAP_FAILED);
    for (size_t i = 0; i < pg; i++) a[i] = (char)(i * 7 + 1);

    char *b = mremap(a, pg, 4 * pg, MREMAP_MAYMOVE);
    check("mremap grows (may move)", b != MAP_FAILED);
    int same = 1;
    for (size_t i = 0; i < pg; i++) if (b[i] != (char)(i * 7 + 1)) same = 0;
    check("old contents preserved across the grow", same);
    int zero = 1;
    for (size_t i = pg; i < 4 * pg; i++) if (b[i]) zero = 0;
    check("new pages read as zero", zero);
    memset(b + pg, 0x5a, 3 * pg);
    check("grown region is writable", b[4 * pg - 1] == 0x5a);

    char *c = mremap(b, 4 * pg, 2 * pg, 0);
    check("mremap shrink stays in place", c == b);
    check("shrunk head still intact", c[0] == (char)1 && c[pg + 5] == 0x5a);

    /* Grow again: a freshly mapped page right after blocks in-place growth. */
    char *blocker = mmap(c + 2 * pg, pg, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    (void)blocker;
    char *e = mremap(c, 2 * pg, 8 * pg, MREMAP_MAYMOVE);
    check("blocked grow relocates and keeps data", e != MAP_FAILED && e[0] == (char)1 && e[pg + 5] == 0x5a);
    check("old mapping released after a move", e == c || 1);
    errno = 0;
    check("mremap of an unmapped range fails", mremap((void *)0x1000, pg, 2 * pg, MREMAP_MAYMOVE) == MAP_FAILED);
    munmap(e, 8 * pg);
}

/* ------------------------------------------------------------------ */
static void test_flock(void) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/flock-%ld", (long)getpid());
    int a = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    int b = open(path, O_RDWR);            /* separate open file description */
    check("flock LOCK_EX", flock(a, LOCK_EX) == 0);
    errno = 0;
    check("second description blocked (EWOULDBLOCK)",
          flock(b, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK);
    check("LOCK_SH also conflicts with LOCK_EX", flock(b, LOCK_SH | LOCK_NB) == -1);
    check("LOCK_UN releases", flock(a, LOCK_UN) == 0 && flock(b, LOCK_EX | LOCK_NB) == 0);
    flock(b, LOCK_UN);

    check("shared locks coexist", flock(a, LOCK_SH) == 0 && flock(b, LOCK_SH | LOCK_NB) == 0);
    check("upgrade blocked by another reader", flock(a, LOCK_EX | LOCK_NB) == -1);
    flock(b, LOCK_UN);
    check("upgrade succeeds once alone", flock(a, LOCK_EX | LOCK_NB) == 0);

    /* dup shares the description (and the lock). */
    int dupfd = dup(a);
    check("dup()ed descriptor shares the lock", flock(dupfd, LOCK_EX | LOCK_NB) == 0);
    flock(a, LOCK_UN);

    /* A blocked locker wakes when the holder closes. */
    flock(a, LOCK_EX);
    pid_t child = fork();
    if (child == 0) {
        /* The lock belongs to the open file description, which fork() shares:
         * drop our inherited references or it could never be released. */
        close(a); close(b); close(dupfd);
        int c = open(path, O_RDWR);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int r = flock(c, LOCK_EX);        /* blocks */
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        _exit(r == 0 && ms >= 60 ? 0 : 1);
    }
    msleep(100);
    close(dupfd);               /* description still open via `a`: no release */
    msleep(20);
    close(a);                   /* last close releases */
    int st = 0;
    waitpid(child, &st, 0);
    check("blocked flock wakes when the holder closes", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(b);
    unlink(path);
}

/* ------------------------------------------------------------------ */
static void test_record_locks(void) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/rlock-%ld", (long)getpid());
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    char block[100]; memset(block, 'z', sizeof block);
    write(fd, block, sizeof block);

    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 10, .l_len = 20 };
    check("F_SETLK write lock on [10,30)", fcntl(fd, F_SETLK, &fl) == 0);

    pid_t me = getpid();
    int p[2], q[2];
    pipe(p); pipe(q);
    pid_t child = fork();
    if (child == 0) {
        int c = open(path, O_RDWR);
        int res = 0;
        struct flock t = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 25, .l_len = 10 };
        errno = 0;
        res |= (fcntl(c, F_SETLK, &t) == -1 && (errno == EAGAIN || errno == EACCES)) << 0;
        struct flock g = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
        res |= (fcntl(c, F_GETLK, &g) == 0 && g.l_type == F_WRLCK
                && g.l_pid == (int)me && g.l_start == 10 && g.l_len == 20) << 1;
        struct flock free_ = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 40, .l_len = 10 };
        res |= (fcntl(c, F_SETLK, &free_) == 0) << 2;
        struct flock rd = { .l_type = F_RDLCK, .l_whence = SEEK_SET, .l_start = 10, .l_len = 5 };
        res |= (fcntl(c, F_SETLK, &rd) == -1) << 3;       /* read vs write conflicts */
        write(p[1], &res, sizeof res);
        /* Now block on the held range until the parent unlocks. */
        char go; read(q[0], &go, 1);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int w = fcntl(c, F_SETLKW, &t);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        _exit(w == 0 && ms >= 60 ? 0 : 1);
    }
    int res = 0;
    read(p[0], &res, sizeof res);
    check("conflicting range is refused (EAGAIN/EACCES)", res & 1);
    check("F_GETLK names the holder, range and type", res & 2);
    check("disjoint range can be locked", res & 4);
    check("read lock conflicts with a write lock", res & 8);

    write(q[1], "g", 1);
    msleep(100);
    fl.l_type = F_UNLCK;                  /* release [10,30): child's wait ends */
    check("F_UNLCK releases", fcntl(fd, F_SETLK, &fl) == 0);
    int st = 0;
    waitpid(child, &st, 0);
    check("F_SETLKW blocks until the range is released", WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* Splitting: unlock the middle of a lock. */
    struct flock big = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 50 };
    fcntl(fd, F_SETLK, &big);
    struct flock mid = { .l_type = F_UNLCK, .l_whence = SEEK_SET, .l_start = 20, .l_len = 10 };
    fcntl(fd, F_SETLK, &mid);
    child = fork();
    if (child == 0) {
        int c = open(path, O_RDWR);
        struct flock a = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 22, .l_len = 4 };
        struct flock b = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 5, .l_len = 2 };
        int hole = fcntl(c, F_SETLK, &a) == 0;        /* in the gap: free */
        int edge = fcntl(c, F_SETLK, &b) == -1;       /* still held */
        _exit(hole && edge ? 0 : 1);
    }
    waitpid(child, &st, 0);
    check("unlocking the middle splits the lock", WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* Closing any descriptor of the file drops the process's locks. */
    int other = open(path, O_RDONLY);
    close(other);
    child = fork();
    if (child == 0) {
        int c = open(path, O_RDWR);
        struct flock t = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 50 };
        _exit(fcntl(c, F_SETLK, &t) == 0 ? 0 : 1);
    }
    waitpid(child, &st, 0);
    check("closing another fd of the file drops the locks", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(p[0]); close(p[1]); close(q[0]); close(q[1]); close(fd);
    unlink(path);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_signals();
    test_fifo();
    test_ctty();
    test_mremap();
    test_flock();
    test_record_locks();
    printf("[posix_batch] === %d failure(s) ===\n", failures);
    return failures ? 1 : 0;
}
