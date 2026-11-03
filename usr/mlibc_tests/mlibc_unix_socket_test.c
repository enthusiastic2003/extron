/* AF_UNIX sockets: socketpair, bind/listen/accept/connect (path + abstract),
 * stream, datagram and seqpacket semantics, sendmsg/recvmsg, poll. */
#define _GNU_SOURCE
#include "arch_compat.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

static int failures;
static void check(const char *what, int ok) {
    printf("[unix_sock] %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void msleep(int ms) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        usleep(1000);
        clock_gettime(CLOCK_MONOTONIC, &t1);
    } while ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 < ms);
}

static socklen_t make_addr(struct sockaddr_un *a, const char *path, int abstract) {
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    if (abstract) {
        a->sun_path[0] = 0;
        strcpy(a->sun_path + 1, path);
        return offsetof(struct sockaddr_un, sun_path) + 1 + strlen(path);
    }
    strcpy(a->sun_path, path);
    return offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1;
}

static void test_pair_stream(void) {
    int sv[2];
    check("socketpair(SOCK_STREAM)", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    check("write on one end", write(sv[0], "hello", 5) == 5);
    char buf[32] = {0};
    check("read on the other end", read(sv[1], buf, sizeof buf) == 5 && !memcmp(buf, "hello", 5));
    check("bidirectional", write(sv[1], "back", 4) == 4 && read(sv[0], buf, sizeof buf) == 4);

    /* stream merges writes */
    write(sv[0], "ab", 2); write(sv[0], "cd", 2);
    memset(buf, 0, sizeof buf);
    check("stream coalesces two writes", read(sv[1], buf, sizeof buf) == 4 && !memcmp(buf, "abcd", 4));

    int n = 0;
    write(sv[0], "12345678", 8);
    check("FIONREAD reports pending bytes", ioctl(sv[1], FIONREAD, &n) == 0 && n == 8);
    check("MSG_PEEK does not consume", recv(sv[1], buf, 3, MSG_PEEK) == 3 && recv(sv[1], buf, 8, 0) == 8);

    errno = 0;
    check("MSG_DONTWAIT on empty is EAGAIN", recv(sv[1], buf, 8, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    int fl = fcntl(sv[1], F_GETFL);
    fcntl(sv[1], F_SETFL, fl | O_NONBLOCK);
    errno = 0;
    check("O_NONBLOCK read is EAGAIN", read(sv[1], buf, 8) == -1 && errno == EAGAIN);
    fcntl(sv[1], F_SETFL, fl);

    struct stat st;
    check("fstat reports S_ISSOCK", fstat(sv[0], &st) == 0 && S_ISSOCK(st.st_mode));
    int type = 0; socklen_t l = sizeof type;
    check("SO_TYPE", getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &type, &l) == 0 && type == SOCK_STREAM);
    struct ucred cr; l = sizeof cr;
    check("SO_PEERCRED names this process",
          getsockopt(sv[0], SOL_SOCKET, SO_PEERCRED, &cr, &l) == 0 && cr.pid == getpid()
          && cr.uid == getuid());

    /* poll */
    struct pollfd pfd = { sv[1], POLLIN, 0 };
    check("poll: nothing to read", poll(&pfd, 1, 0) == 0);
    write(sv[0], "x", 1);
    check("poll: POLLIN after a write", poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN));
    read(sv[1], buf, 1);

    /* shutdown */
    check("shutdown(SHUT_WR) succeeds", shutdown(sv[0], SHUT_WR) == 0);
    check("peer reads EOF after SHUT_WR", read(sv[1], buf, 8) == 0);
    check("peer can still write back", write(sv[1], "z", 1) == 1 && read(sv[0], buf, 1) == 1);

    /* EOF and EPIPE on close */
    close(sv[0]);
    check("read returns EOF after the peer closes", read(sv[1], buf, 8) == 0);
    signal(SIGPIPE, SIG_IGN);
    errno = 0;
    check("write to a closed peer is EPIPE", write(sv[1], "x", 1) == -1 && errno == EPIPE);
    pfd.fd = sv[1]; pfd.events = POLLIN; pfd.revents = 0;
    check("poll reports POLLHUP", poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLHUP));
    close(sv[1]);
}

static void test_pair_dgram(void) {
    int sv[2];
    check("socketpair(SOCK_DGRAM)", socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
    send(sv[0], "one", 3, 0);
    send(sv[0], "second", 6, 0);
    char buf[32] = {0};
    check("datagram boundaries preserved (1)", recv(sv[1], buf, sizeof buf, 0) == 3 && !memcmp(buf, "one", 3));
    memset(buf, 0, sizeof buf);
    check("datagram boundaries preserved (2)", recv(sv[1], buf, sizeof buf, 0) == 6 && !memcmp(buf, "second", 6));
    send(sv[0], "truncate-me", 11, 0);
    ssize_t r = recv(sv[1], buf, 4, 0);
    check("short read of a datagram drops the rest", r == 4 && recv(sv[1], buf, sizeof buf, MSG_DONTWAIT) == -1);

    struct iovec out[3] = { {"ab", 2}, {"", 0}, {"cdef", 4} };
    struct msghdr mh = { .msg_iov = out, .msg_iovlen = 3 };
    check("sendmsg gathers iovecs", sendmsg(sv[0], &mh, 0) == 6);
    char a[3] = {0}, b[8] = {0};
    struct iovec in[2] = { {a, 3}, {b, 8} };
    struct msghdr rh = { .msg_iov = in, .msg_iovlen = 2 };
    check("recvmsg scatters iovecs", recvmsg(sv[1], &rh, 0) == 6 && !memcmp(a, "abc", 3) && !memcmp(b, "def", 3));
    struct iovec tiny = { a, 2 };
    struct msghdr th = { .msg_iov = &tiny, .msg_iovlen = 1 };
    send(sv[0], "toolong", 7, 0);
    check("recvmsg flags MSG_TRUNC", recvmsg(sv[1], &th, 0) == 2 && (th.msg_flags & MSG_TRUNC));
    close(sv[0]); close(sv[1]);
}

static void test_path_stream(void) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/usock-%ld", (long)getpid());
    unlink(path);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    socklen_t alen = make_addr(&addr, path, 0);
    check("bind to a filesystem path", bind(srv, (struct sockaddr *)&addr, alen) == 0);
    struct stat st;
    check("bind creates a socket file", stat(path, &st) == 0 && S_ISSOCK(st.st_mode));
    int dup_srv = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    check("second bind to the same path is EADDRINUSE",
          bind(dup_srv, (struct sockaddr *)&addr, alen) == -1 && errno == EADDRINUSE);
    close(dup_srv);
    errno = 0;
    check("open() of a socket file is ENXIO", open(path, O_RDWR) == -1 && errno == ENXIO);

    int c0 = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    check("connect before listen is ECONNREFUSED",
          connect(c0, (struct sockaddr *)&addr, alen) == -1 && errno == ECONNREFUSED);
    close(c0);
    check("listen", listen(srv, 4) == 0);

    struct sockaddr_un got; socklen_t gl = sizeof got;
    check("getsockname returns the bound path",
          getsockname(srv, (struct sockaddr *)&got, &gl) == 0 && !strcmp(got.sun_path, path));

    pid_t child = fork();
    if (child == 0) {
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(c, (struct sockaddr *)&addr, alen) != 0) _exit(1);
        char buf[16];
        if (write(c, "from-client", 11) != 11) _exit(2);
        ssize_t n = read(c, buf, sizeof buf);
        if (n != 10 || memcmp(buf, "from-serve", 10)) _exit(3);
        struct sockaddr_un peer; socklen_t pl = sizeof peer;
        if (getpeername(c, (struct sockaddr *)&peer, &pl) != 0 || strcmp(peer.sun_path, path)) _exit(4);
        _exit(0);
    }
    struct pollfd pfd = { srv, POLLIN, 0 };
    check("poll(listener) signals a pending connection", poll(&pfd, 1, 2000) == 1 && (pfd.revents & POLLIN));
    struct sockaddr_un from; socklen_t fl = sizeof from;
    int conn = accept(srv, (struct sockaddr *)&from, &fl);
    check("accept returns a connected socket", conn >= 0);
    char buf[32] = {0};
    check("server reads the client's data", read(conn, buf, sizeof buf) == 11 && !memcmp(buf, "from-client", 11));
    write(conn, "from-server", 10);
    int status = 0;
    waitpid(child, &status, 0);
    check("client round trip (incl. getpeername)", WIFEXITED(status) && WEXITSTATUS(status) == 0);
    check("EOF at the server after the client exits", read(conn, buf, sizeof buf) == 0);
    close(conn);

    /* Backlog: three clients queued before accept. */
    int cs[3];
    for (int i = 0; i < 3; i++) {
        cs[i] = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(cs[i], (struct sockaddr *)&addr, alen) != 0) cs[i] = -1;
        if (cs[i] >= 0) write(cs[i], (char[]){ 'A' + i }, 1);
    }
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        int a = accept(srv, NULL, NULL);
        char c = 0;
        if (a < 0 || read(a, &c, 1) != 1 || c != 'A' + i) ok = 0;
        if (a >= 0) close(a);
    }
    check("backlogged connections accepted in order", ok);
    for (int i = 0; i < 3; i++) if (cs[i] >= 0) close(cs[i]);

    int fl2 = fcntl(srv, F_GETFL);
    fcntl(srv, F_SETFL, fl2 | O_NONBLOCK);
    errno = 0;
    check("nonblocking accept with nothing queued is EAGAIN",
          accept(srv, NULL, NULL) == -1 && errno == EAGAIN);

    close(srv);
    int c1 = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    check("connect to a closed listener is ECONNREFUSED",
          connect(c1, (struct sockaddr *)&addr, alen) == -1 && errno == ECONNREFUSED);
    close(c1);
    check("unlink removes the socket file", unlink(path) == 0 && stat(path, &st) == -1);
}

static void test_abstract(void) {
    struct sockaddr_un addr;
    char name[40];
    snprintf(name, sizeof name, "abs-%ld", (long)getpid());
    socklen_t alen = make_addr(&addr, name, 1);
    int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check("bind in the abstract namespace", bind(srv, (struct sockaddr *)&addr, alen) == 0);
    listen(srv, 2);
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    check("connect to an abstract name", connect(c, (struct sockaddr *)&addr, alen) == 0);
    int a = accept(srv, NULL, NULL);
    check("accept on an abstract listener", a >= 0 && write(c, "k", 1) == 1);
    char ch = 0;
    check("data flows over the abstract connection", a >= 0 && read(a, &ch, 1) == 1 && ch == 'k');
    int sock_type_opt = 0; socklen_t l = sizeof sock_type_opt;
    check("SO_ACCEPTCONN on a listener", getsockopt(srv, SOL_SOCKET, SO_ACCEPTCONN, &sock_type_opt, &l) == 0 && sock_type_opt == 1);
    close(a); close(c); close(srv);
}

static void test_dgram_named(void) {
    char p1[64], p2[64];
    snprintf(p1, sizeof p1, "/tmp/udg1-%ld", (long)getpid());
    snprintf(p2, sizeof p2, "/tmp/udg2-%ld", (long)getpid());
    unlink(p1); unlink(p2);
    int s1 = socket(AF_UNIX, SOCK_DGRAM, 0), s2 = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un a1, a2;
    socklen_t l1 = make_addr(&a1, p1, 0), l2 = make_addr(&a2, p2, 0);
    check("bind two datagram sockets", bind(s1, (struct sockaddr *)&a1, l1) == 0
          && bind(s2, (struct sockaddr *)&a2, l2) == 0);
    check("sendto a named socket", sendto(s1, "ping", 4, 0, (struct sockaddr *)&a2, l2) == 4);
    char buf[16] = {0};
    struct sockaddr_un from; socklen_t fl = sizeof from;
    check("recvfrom reports the sender's address",
          recvfrom(s2, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl) == 4
          && !memcmp(buf, "ping", 4) && !strcmp(from.sun_path, p1));
    check("connect() sets the default destination", connect(s2, (struct sockaddr *)&a1, l1) == 0
          && send(s2, "pong", 4, 0) == 4 && recv(s1, buf, sizeof buf, 0) == 4);
    errno = 0;
    int s3 = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un nowhere; socklen_t ln = make_addr(&nowhere, "/tmp/no-such-socket", 0);
    check("sendto a missing path fails", sendto(s3, "x", 1, 0, (struct sockaddr *)&nowhere, ln) == -1);
    close(s1); close(s2); close(s3);
    unlink(p1); unlink(p2);
}

static void test_seqpacket(void) {
    int sv[2];
    check("socketpair(SOCK_SEQPACKET)", socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
    send(sv[0], "msg-1", 5, 0);
    send(sv[0], "msg-two", 7, 0);
    char buf[32] = {0};
    check("seqpacket keeps message boundaries",
          recv(sv[1], buf, sizeof buf, 0) == 5 && recv(sv[1], buf, sizeof buf, 0) == 7);
    close(sv[0]);
    check("seqpacket EOF after the peer closes", recv(sv[1], buf, sizeof buf, 0) == 0);
    close(sv[1]);
}

/* Push 1 MiB through a stream between two processes: exercises flow control
 * (the kernel buffer is far smaller) and blocking in both directions. */
static void test_bulk(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    const size_t total = 1 << 20;
    pid_t child = fork();
    if (child == 0) {
        close(sv[0]);
        unsigned char chunk[3000];
        size_t sent = 0;
        while (sent < total) {
            size_t n = sizeof chunk < total - sent ? sizeof chunk : total - sent;
            for (size_t i = 0; i < n; i++) chunk[i] = (unsigned char)((sent + i) * 31 + 7);
            ssize_t w = write(sv[1], chunk, n);
            if (w <= 0) _exit(1);
            /* a short write must still be consistent: resend the remainder */
            size_t done = (size_t)w;
            while (done < n) {
                ssize_t w2 = write(sv[1], chunk + done, n - done);
                if (w2 <= 0) _exit(2);
                done += (size_t)w2;
            }
            sent += n;
        }
        _exit(0);
    }
    close(sv[1]);
    unsigned char buf[4096];
    size_t got = 0; int bad = 0;
    for (;;) {
        ssize_t n = read(sv[0], buf, sizeof buf);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++)
            if (buf[i] != (unsigned char)((got + i) * 31 + 7)) bad++;
        got += (size_t)n;
    }
    int status = 0;
    waitpid(child, &status, 0);
    check("1 MiB transfer arrives complete", got == total);
    check("1 MiB transfer arrives intact", bad == 0);
    check("sender exits cleanly", WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(sv[0]);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_pair_stream();
    test_pair_dgram();
    test_path_stream();
    test_abstract();
    test_dgram_named();
    test_seqpacket();
    test_bulk();
    printf("[unix_sock] === %d failure(s) ===\n", failures);
    return failures ? 1 : 0;
}
