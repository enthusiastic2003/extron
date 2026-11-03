/* epoll, eventfd, timerfd, signalfd. */
#define _GNU_SOURCE
#include "arch_compat.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/wait.h>

static int failures;
static void check(const char *what, int ok) {
    printf("[epoll_test] %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void msleep(int ms) {
    long end = now_ms() + ms;
    while (now_ms() < end) usleep(1000);
}

static void test_eventfd(void) {
    int e = eventfd(3, EFD_NONBLOCK);
    check("eventfd(3)", e >= 0);
    uint64_t v = 0;
    check("read returns and clears the counter", read(e, &v, 8) == 8 && v == 3);
    errno = 0;
    check("empty nonblocking read is EAGAIN", read(e, &v, 8) == -1 && errno == EAGAIN);
    v = 5; write(e, &v, 8); v = 7; write(e, &v, 8);
    check("writes accumulate", read(e, &v, 8) == 8 && v == 12);
    errno = 0;
    check("short read is EINVAL", read(e, &v, 4) == -1 && errno == EINVAL);
    struct pollfd p = { e, POLLIN | POLLOUT, 0 };
    poll(&p, 1, 0);
    check("poll: writable but not readable when zero", (p.revents & POLLOUT) && !(p.revents & POLLIN));
    v = 1; write(e, &v, 8);
    poll(&p, 1, 0);
    check("poll: readable once non-zero", p.revents & POLLIN);
    close(e);

    int s = eventfd(0, EFD_SEMAPHORE | EFD_NONBLOCK);
    v = 3; write(s, &v, 8);
    uint64_t a = 0, b = 0, c = 0;
    read(s, &a, 8); read(s, &b, 8); read(s, &c, 8);
    check("EFD_SEMAPHORE returns 1 per read", a == 1 && b == 1 && c == 1
          && read(s, &a, 8) == -1);
    close(s);

    /* blocking read woken by another process */
    int w = eventfd(0, 0);
    pid_t child = fork();
    if (child == 0) {
        msleep(60);
        uint64_t one = 9;
        write(w, &one, 8);
        _exit(0);
    }
    long t0 = now_ms();
    v = 0;
    ssize_t r = read(w, &v, 8);
    check("blocking read wakes when written elsewhere", r == 8 && v == 9 && now_ms() - t0 >= 40);
    waitpid(child, NULL, 0);
    close(w);
}

static void test_epoll_basic(void) {
    int ep = epoll_create1(EPOLL_CLOEXEC);
    check("epoll_create1", ep >= 0);
    int p[2];
    pipe(p);
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0xabcdef };
    check("EPOLL_CTL_ADD a pipe", epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
    errno = 0;
    check("duplicate ADD is EEXIST", epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == -1 && errno == EEXIST);
    struct epoll_event out[4];
    check("nothing ready: timeout 0 returns 0", epoll_wait(ep, out, 4, 0) == 0);
    long t0 = now_ms();
    check("timed wait returns 0 after the timeout", epoll_wait(ep, out, 4, 80) == 0 && now_ms() - t0 >= 60);

    write(p[1], "x", 1);
    int n = epoll_wait(ep, out, 4, 1000);
    check("ready fd reported with its data", n == 1 && out[0].data.u64 == 0xabcdef && (out[0].events & EPOLLIN));
    check("level-triggered: reported again", epoll_wait(ep, out, 4, 0) == 1);
    char c; read(p[0], &c, 1);
    check("not reported once drained", epoll_wait(ep, out, 4, 0) == 0);

    /* MOD / DEL */
    ev.events = EPOLLIN | EPOLLOUT; ev.data.u64 = 7;
    check("EPOLL_CTL_MOD", epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) == 0);
    check("EPOLL_CTL_DEL", epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL) == 0);
    write(p[1], "y", 1);
    check("deleted fd no longer reported", epoll_wait(ep, out, 4, 0) == 0);
    errno = 0;
    check("DEL of an unknown fd is ENOENT", epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL) == -1 && errno == ENOENT);

    /* regular files are not pollable */
    int rf = open("/opt/tests/hello.txt", O_RDONLY);
    errno = 0;
    check("ADD of a regular file is EPERM", epoll_ctl(ep, EPOLL_CTL_ADD, rf, &ev) == -1 && errno == EPERM);
    close(rf);

    /* writer end: POLLOUT; HUP when the reader closes */
    struct epoll_event wev = { .events = EPOLLOUT, .data.fd = p[1] };
    epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &wev);
    n = epoll_wait(ep, out, 4, 0);
    check("pipe write end is writable", n == 1 && (out[0].events & EPOLLOUT));
    close(p[0]);
    n = epoll_wait(ep, out, 4, 0);
    check("EPOLLERR once the reader closes", n == 1 && (out[0].events & EPOLLERR));

    /* closing the last descriptor drops the registration */
    int q[2]; pipe(q);
    struct epoll_event qe = { .events = EPOLLIN, .data.fd = q[0] };
    epoll_ctl(ep, EPOLL_CTL_ADD, q[0], &qe);
    write(q[1], "z", 1);
    close(q[0]);
    check("closing the fd removes its registration", epoll_wait(ep, out, 4, 0) == 0
          || out[0].data.fd != q[0]);
    close(q[1]); close(p[1]); close(ep);
}

static void test_epoll_modes(void) {
    int ep = epoll_create1(0);
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.fd = sv[0] };
    epoll_ctl(ep, EPOLL_CTL_ADD, sv[0], &ev);
    struct epoll_event out[2];
    write(sv[1], "aaaa", 4);
    check("EPOLLET reports the first edge", epoll_wait(ep, out, 2, 0) == 1);
    check("EPOLLET does not repeat while still readable", epoll_wait(ep, out, 2, 0) == 0);
    char buf[8];
    read(sv[0], buf, 8);
    write(sv[1], "b", 1);
    check("EPOLLET reports a fresh edge", epoll_wait(ep, out, 2, 0) == 1);
    read(sv[0], buf, 8);

    ev.events = EPOLLIN | EPOLLONESHOT;
    epoll_ctl(ep, EPOLL_CTL_MOD, sv[0], &ev);
    write(sv[1], "c", 1);
    check("EPOLLONESHOT fires once", epoll_wait(ep, out, 2, 0) == 1 && epoll_wait(ep, out, 2, 0) == 0);
    epoll_ctl(ep, EPOLL_CTL_MOD, sv[0], &ev);
    check("MOD re-arms a oneshot", epoll_wait(ep, out, 2, 0) == 1);
    read(sv[0], buf, 8);

    /* RDHUP / HUP on peer close */
    ev.events = EPOLLIN | EPOLLRDHUP;
    epoll_ctl(ep, EPOLL_CTL_MOD, sv[0], &ev);
    close(sv[1]);
    int n = epoll_wait(ep, out, 2, 100);
    check("peer close gives EPOLLIN|EPOLLRDHUP|EPOLLHUP",
          n == 1 && (out[0].events & EPOLLIN) && (out[0].events & EPOLLRDHUP) && (out[0].events & EPOLLHUP));
    close(sv[0]);

    /* several ready at once, limited by maxevents */
    int ps[3][2];
    for (int i = 0; i < 3; i++) {
        pipe(ps[i]);
        struct epoll_event e = { .events = EPOLLIN, .data.u32 = (uint32_t)i };
        epoll_ctl(ep, EPOLL_CTL_ADD, ps[i][0], &e);
        write(ps[i][1], "x", 1);
    }
    struct epoll_event many[8];
    check("all ready fds reported", epoll_wait(ep, many, 8, 0) == 3);
    check("maxevents bounds the result", epoll_wait(ep, many, 2, 0) == 2);
    for (int i = 0; i < 3; i++) { close(ps[i][0]); close(ps[i][1]); }
    close(ep);
}

static void test_blocking_wake(void) {
    int ep = epoll_create1(0);
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = sv[0] };
    epoll_ctl(ep, EPOLL_CTL_ADD, sv[0], &ev);
    pid_t child = fork();
    if (child == 0) {
        msleep(70);
        write(sv[1], "hi", 2);
        _exit(0);
    }
    long t0 = now_ms();
    struct epoll_event out[2];
    int n = epoll_wait(ep, out, 2, -1);
    check("infinite epoll_wait wakes on data from another process", n == 1 && now_ms() - t0 >= 50);
    waitpid(child, NULL, 0);
    close(sv[0]); close(sv[1]); close(ep);
}

static void on_usr1(int s) { (void)s; }

static void test_pwait_mask(void) {
    int ep = epoll_create1(0);
    int p[2]; pipe(p);
    struct epoll_event ev = { .events = EPOLLIN };
    epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    sigset_t block, old, none;
    sigemptyset(&block); sigaddset(&block, SIGUSR1);
    sigemptyset(&none);
    sigprocmask(SIG_BLOCK, &block, &old);
    pid_t me = getpid();
    pid_t child = fork();
    if (child == 0) { msleep(60); kill(me, SIGUSR1); _exit(0); }
    struct epoll_event out[1];
    errno = 0;
    int n = epoll_pwait(ep, out, 1, 2000, &none);
    check("epoll_pwait unblocks the signal and returns EINTR", n == -1 && errno == EINTR);
    sigset_t cur; sigprocmask(SIG_BLOCK, NULL, &cur);
    check("the original mask is back afterwards", sigismember(&cur, SIGUSR1));
    waitpid(child, NULL, 0);
    sigprocmask(SIG_SETMASK, &old, NULL);
    close(p[0]); close(p[1]); close(ep);
}

static void test_timerfd(void) {
    int t = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    check("timerfd_create", t >= 0);
    uint64_t n = 0;
    errno = 0;
    check("unarmed timer read is EAGAIN", read(t, &n, 8) == -1 && errno == EAGAIN);
    struct itimerspec its = { .it_value = { 0, 60 * 1000000L } };
    check("timerfd_settime one-shot 60ms", timerfd_settime(t, 0, &its, NULL) == 0);
    struct itimerspec cur;
    timerfd_gettime(t, &cur);
    check("gettime reports time remaining", cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec > 0
          && cur.it_value.tv_nsec <= 60 * 1000000L);
    errno = 0;
    check("not yet expired", read(t, &n, 8) == -1 && errno == EAGAIN);
    msleep(90);
    check("expired once", read(t, &n, 8) == 8 && n == 1);
    check("one-shot does not repeat", read(t, &n, 8) == -1);

    /* periodic: count expirations */
    its.it_value.tv_nsec = 20 * 1000000L;
    its.it_interval.tv_nsec = 20 * 1000000L;
    timerfd_settime(t, 0, &its, NULL);
    msleep(110);
    n = 0;
    check("periodic timer accumulates expirations", read(t, &n, 8) == 8 && n >= 4 && n <= 7);
    its.it_value.tv_nsec = 0; its.it_interval.tv_nsec = 0;
    timerfd_settime(t, 0, &its, NULL);
    msleep(40);
    check("disarming stops it", read(t, &n, 8) == -1);
    close(t);

    /* blocking read sleeps until expiry */
    int b = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec slow = { .it_value = { 0, 80 * 1000000L } };
    timerfd_settime(b, 0, &slow, NULL);
    long t0 = now_ms();
    check("blocking timerfd read waits for the expiry",
          read(b, &n, 8) == 8 && now_ms() - t0 >= 60 && now_ms() - t0 < 400);
    close(b);

    /* absolute time on CLOCK_REALTIME */
    int r = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
    struct timespec nowts; clock_gettime(CLOCK_REALTIME, &nowts);
    struct itimerspec abs = { .it_value = { nowts.tv_sec, nowts.tv_nsec + 50 * 1000000L } };
    if (abs.it_value.tv_nsec >= 1000000000L) { abs.it_value.tv_sec++; abs.it_value.tv_nsec -= 1000000000L; }
    timerfd_settime(r, TFD_TIMER_ABSTIME, &abs, NULL);
    msleep(90);
    check("TFD_TIMER_ABSTIME on CLOCK_REALTIME", read(r, &n, 8) == 8 && n == 1);
    close(r);
}

static void test_timerfd_epoll(void) {
    int ep = epoll_create1(0);
    int t = timerfd_create(CLOCK_MONOTONIC, 0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = t };
    epoll_ctl(ep, EPOLL_CTL_ADD, t, &ev);
    struct itimerspec its = { .it_value = { 0, 70 * 1000000L } };
    timerfd_settime(t, 0, &its, NULL);
    long t0 = now_ms();
    struct epoll_event out[1];
    int n = epoll_wait(ep, out, 1, -1);          /* nothing else will wake us */
    check("epoll_wait(-1) wakes for a timerfd expiry", n == 1 && out[0].data.fd == t
          && now_ms() - t0 >= 50 && now_ms() - t0 < 500);
    uint64_t cnt; read(t, &cnt, 8);

    struct pollfd p = { t, POLLIN, 0 };
    timerfd_settime(t, 0, &its, NULL);
    t0 = now_ms();
    check("poll(-1) also wakes for a timerfd", poll(&p, 1, -1) == 1 && now_ms() - t0 >= 50);
    close(t); close(ep);
}

static void test_signalfd(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sfd = signalfd(-1, &mask, SFD_NONBLOCK);
    check("signalfd", sfd >= 0);
    struct signalfd_siginfo si;
    errno = 0;
    check("no signal pending: EAGAIN", read(sfd, &si, sizeof si) == -1 && errno == EAGAIN);
    kill(getpid(), SIGUSR1);
    struct pollfd p = { sfd, POLLIN, 0 };
    check("poll: POLLIN when a masked signal is pending", poll(&p, 1, 0) == 1 && (p.revents & POLLIN));
    memset(&si, 0, sizeof si);
    check("read yields the siginfo", read(sfd, &si, sizeof si) == sizeof si
          && si.ssi_signo == SIGUSR1 && si.ssi_pid == (uint32_t)getpid());
    check("signal consumed", read(sfd, &si, sizeof si) == -1);

    /* SIGCHLD via signalfd and epoll */
    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = sfd };
    epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &ev);
    pid_t child = fork();
    if (child == 0) { msleep(50); _exit(3); }
    struct epoll_event out[1];
    int n = epoll_wait(ep, out, 1, 2000);
    check("epoll sees SIGCHLD through the signalfd", n == 1);
    memset(&si, 0, sizeof si);
    ssize_t r = read(sfd, &si, sizeof si);
    check("SIGCHLD siginfo", r == sizeof si && si.ssi_signo == SIGCHLD && si.ssi_pid == (uint32_t)child);
    waitpid(child, NULL, 0);

    /* blocking read */
    int bfd = signalfd(-1, &mask, 0);
    pid_t me = getpid();
    child = fork();
    if (child == 0) { msleep(50); kill(me, SIGUSR1); _exit(0); }
    long t0 = now_ms();
    r = read(bfd, &si, sizeof si);
    check("blocking signalfd read waits for the signal", r == sizeof si && si.ssi_signo == SIGUSR1
          && now_ms() - t0 >= 30);
    waitpid(child, NULL, 0);
    /* drain the SIGCHLD from that child */
    while (read(sfd, &si, sizeof si) == sizeof si) {}
    close(bfd); close(sfd); close(ep);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_eventfd();
    test_epoll_basic();
    test_epoll_modes();
    test_blocking_wake();
    test_pwait_mask();
    test_timerfd();
    test_timerfd_epoll();
    test_signalfd();
    printf("[epoll_test] === %d failure(s) ===\n", failures);
    return failures ? 1 : 0;
}
