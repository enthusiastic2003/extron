/* SCM_RIGHTS: passing open file descriptions over AF_UNIX stream sockets (what OpenSSH's
 * privilege-separated sshd does to hand a pty to the session process). */
#define _GNU_SOURCE
#include "arch_compat.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>

static int failures;
static void check(const char *what, int ok) {
    printf("[scm_rights] %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* sendmsg() carrying `n` descriptors plus `len` data bytes. */
static ssize_t send_fds(int sock, const void *data, size_t len, const int *fds, int n) {
    struct iovec iov = { (void *)data, len };
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int) * 8)]; } u;
    struct msghdr m;
    memset(&m, 0, sizeof(m));
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    if (n) {
        memset(&u, 0, sizeof(u));
        m.msg_control = u.buf;
        m.msg_controllen = CMSG_SPACE(sizeof(int) * n);
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * n);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * n);
    }
    return sendmsg(sock, &m, 0);
}

/* recvmsg(): returns bytes, fills fds[] and *nfds (capacity `cap`), *flags = msg_flags. */
static ssize_t recv_fds(int sock, void *data, size_t len, int *fds, int *nfds, int cap,
                        int *flags, int rflags) {
    struct iovec iov = { data, len };
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int) * 8)]; } u;
    struct msghdr m;
    memset(&m, 0, sizeof(m));
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = u.buf;
    m.msg_controllen = cap > 0 ? CMSG_SPACE(sizeof(int) * cap) : 0;
    ssize_t r = recvmsg(sock, &m, rflags);
    *nfds = 0;
    if (r >= 0) {
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c && m.msg_controllen; c = CMSG_NXTHDR(&m, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                int cnt = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                memcpy(fds + *nfds, CMSG_DATA(c), sizeof(int) * cnt);
                *nfds += cnt;
            }
        }
        if (flags) *flags = m.msg_flags;
    }
    return r;
}

/* True once every writer of the pipe whose read end is `rfd` is gone (read() returns 0). */
static int pipe_at_eof(int rfd) {
    int fl = fcntl(rfd, F_GETFL);
    fcntl(rfd, F_SETFL, fl | O_NONBLOCK);
    char c;
    ssize_t r = read(rfd, &c, 1);
    fcntl(rfd, F_SETFL, fl);
    return r == 0;
}

int main(void) {
    int sv[2];
    char buf[64];
    int fds[8], n, mflags;

    check("socketpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    /* 1. a pipe end travels and still talks to the other end */
    int p[2];
    pipe(p);
    check("send one descriptor (pipe read end)", send_fds(sv[0], "x", 1, &p[0], 1) == 1);
    check("recv it", recv_fds(sv[1], buf, sizeof(buf), fds, &n, 4, &mflags, 0) == 1 && n == 1
                     && buf[0] == 'x' && !(mflags & MSG_CTRUNC));
    int got = n == 1 ? fds[0] : -1;
    check("received number is a new descriptor", got >= 0 && got != p[0]);
    close(p[0]);                                    /* the sender's copy goes away: still alive */
    write(p[1], "hello", 5);
    memset(buf, 0, sizeof(buf));
    check("data written to the pipe arrives through the passed fd",
          read(got, buf, 5) == 5 && memcmp(buf, "hello", 5) == 0);
    close(p[1]);
    check("EOF follows once the last writer closes", pipe_at_eof(got));
    close(got);

    /* 2. several descriptors in one message keep their order */
    int q[3][2];
    for (int i = 0; i < 3; i++) pipe(q[i]);
    int three[3] = { q[0][0], q[1][0], q[2][0] };
    check("send three descriptors", send_fds(sv[0], "abc", 3, three, 3) == 3);
    check("recv three", recv_fds(sv[1], buf, sizeof(buf), fds, &n, 4, &mflags, 0) == 3 && n == 3);
    int order_ok = n == 3;
    for (int i = 0; i < n; i++) {
        char c = (char)('0' + i);
        write(q[i][1], &c, 1);
        char r = 0;
        if (read(fds[i], &r, 1) != 1 || r != c) order_ok = 0;
    }
    check("descriptors arrive in order", order_ok);
    for (int i = 0; i < 3; i++) { close(q[i][0]); close(q[i][1]); if (i < n) close(fds[i]); }

    /* 3. message boundaries: one recv never spans two messages that carry descriptors */
    pipe(p);
    send_fds(sv[0], "A", 1, &p[0], 1);
    send_fds(sv[0], "BC", 2, NULL, 0);
    int r1 = (int)recv_fds(sv[1], buf, 3, fds, &n, 4, &mflags, 0);
    check("recv stops at the message with the descriptor", r1 == 1 && buf[0] == 'A' && n == 1);
    if (n == 1) close(fds[0]);
    int r2 = (int)recv_fds(sv[1], buf, 3, fds, &n, 4, &mflags, 0);
    check("next recv gets the plain bytes with no descriptors", r2 == 2 && n == 0);
    close(p[0]); close(p[1]);

    /* 4. plain read() discards the descriptors (and the description is released) */
    pipe(p);
    int wr = p[1];
    send_fds(sv[0], "Z", 1, &wr, 1);
    close(p[1]);                                    /* only the in-flight copy keeps it open */
    check("writer still open while in flight", !pipe_at_eof(p[0]));
    check("plain read() takes the byte", read(sv[1], buf, 1) == 1 && buf[0] == 'Z');
    check("discarded descriptor released (reader sees EOF)", pipe_at_eof(p[0]));
    close(p[0]);

    /* 5. recvmsg with no control buffer behaves the same */
    pipe(p);
    wr = p[1];
    send_fds(sv[0], "Y", 1, &wr, 1);
    close(p[1]);
    check("recvmsg without control space", recv_fds(sv[1], buf, 1, fds, &n, 0, &mflags, 0) == 1
                                           && n == 0);
    check("descriptor released and MSG_CTRUNC set", pipe_at_eof(p[0]) && (mflags & MSG_CTRUNC));
    close(p[0]);

    /* 6. control buffer too small for all of them: extras are closed, MSG_CTRUNC */
    int w4[4], r4[4];
    for (int i = 0; i < 4; i++) { int pp[2]; pipe(pp); r4[i] = pp[0]; w4[i] = pp[1]; }
    send_fds(sv[0], "t", 1, w4, 4);
    for (int i = 0; i < 4; i++) close(w4[i]);
    check("recv with a small control buffer", recv_fds(sv[1], buf, 1, fds, &n, 1, &mflags, 0) == 1
                                              && n >= 1 && n < 4 && (mflags & MSG_CTRUNC));
    int kept = 0, closed_ok = 1;
    for (int i = 0; i < 4; i++) {
        if (i < n) { if (pipe_at_eof(r4[i])) closed_ok = 0; else kept++; }
        else if (!pipe_at_eof(r4[i])) closed_ok = 0;
    }
    check("the first n are kept, the rest released", kept == n && closed_ok);
    for (int i = 0; i < 4; i++) close(r4[i]);
    for (int i = 0; i < n; i++) close(fds[i]);

    /* 7. MSG_CMSG_CLOEXEC */
    pipe(p);
    send_fds(sv[0], "c", 1, &p[0], 1);
    recv_fds(sv[1], buf, 1, fds, &n, 1, &mflags, MSG_CMSG_CLOEXEC);
    check("MSG_CMSG_CLOEXEC sets FD_CLOEXEC", n == 1 && (fcntl(fds[0], F_GETFD) & FD_CLOEXEC));
    if (n == 1) close(fds[0]);
    close(p[0]); close(p[1]);

    /* 8. errors */
    errno = 0;
    int bad = 999;
    check("invalid descriptor -> EBADF", send_fds(sv[0], "e", 1, &bad, 1) < 0 && errno == EBADF);
    int dg[2];
    socketpair(AF_UNIX, SOCK_DGRAM, 0, dg);
    pipe(p);
    errno = 0;
    check("datagram sockets refuse descriptors (EOPNOTSUPP)",
          send_fds(dg[0], "d", 1, &p[0], 1) < 0 && errno == EOPNOTSUPP);
    check("... and the refused descriptor is not leaked or consumed",
          write(p[1], "k", 1) == 1 && read(p[0], buf, 1) == 1);
    close(p[0]); close(p[1]); close(dg[0]); close(dg[1]);

    /* 9. receiver closes with descriptors still queued */
    int sv2[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv2);
    pipe(p);
    wr = p[1];
    send_fds(sv2[0], "q", 1, &wr, 1);
    close(p[1]);
    check("writer held by the queue", !pipe_at_eof(p[0]));
    close(sv2[1]);
    check("closing the receiving socket releases the queued descriptor", pipe_at_eof(p[0]));
    close(p[0]); close(sv2[0]);

    /* 10. a socket can itself be passed and used */
    int inner[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, inner);
    send_fds(sv[0], "s", 1, &inner[1], 1);
    recv_fds(sv[1], buf, 1, fds, &n, 1, &mflags, 0);
    int ok = n == 1;
    if (ok) {
        write(fds[0], "ping", 4);
        memset(buf, 0, sizeof(buf));
        ok = read(inner[0], buf, 4) == 4 && memcmp(buf, "ping", 4) == 0;
        close(fds[0]);
    }
    check("a passed socket works", ok);
    close(inner[0]); close(inner[1]);

    /* 11. across fork: the child receives what the parent sends */
    pipe(p);
    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        int cf[4], cn;
        if (recv_fds(sv[1], buf, 1, cf, &cn, 4, NULL, 0) != 1 || cn != 1)
            _exit(2);
        char c = 0;
        if (read(cf[0], &c, 1) != 1 || c != 'K')
            _exit(3);
        _exit(0);
    }
    close(sv[1]);
    send_fds(sv[0], "f", 1, &p[0], 1);
    close(p[0]);
    write(p[1], "K", 1);
    int st = 0;
    waitpid(pid, &st, 0);
    check("child process receives and uses the descriptor", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(p[1]);

    printf("[scm_rights] %s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
