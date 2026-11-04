/*
 * AF_INET sockets over the loopback interface (kernel/net/inet_sock.c on lwIP): TCP
 * echo with a large transfer, half-close, UDP datagrams, poll(), non-blocking
 * connect, refused connections, address and option queries. No NIC is needed.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

static void check(const char *what, int ok) {
    printf("[net_test] %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static struct sockaddr_in loopback(unsigned short port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

static unsigned short bound_port(int fd) {
    struct sockaddr_in a;
    socklen_t l = sizeof(a);
    return getsockname(fd, (struct sockaddr *)&a, &l) == 0 ? ntohs(a.sin_port) : 0;
}

static int listener(int backlog) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = loopback(0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, backlog) != 0) {
        close(s);
        return -1;
    }
    return s;
}

/* Deterministic payload so both ends can verify without sharing memory. */
static unsigned char pat(size_t i) { return (unsigned char)((i * 131u + (i >> 8)) & 0xFF); }

#define BIG (512 * 1024)

static int write_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t n) {
    unsigned char *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    /* ---- socket basics ---- */
    int ls = listener(8);
    check("TCP socket, bind to 127.0.0.1:0, listen", ls >= 0);
    unsigned short port = bound_port(ls);
    check("bind picked an ephemeral port", port != 0);
    int one = 1;
    check("setsockopt SO_REUSEADDR / TCP_NODELAY accepted",
          setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
    int type = 0; socklen_t tl = sizeof(type);
    check("getsockopt SO_TYPE is SOCK_STREAM",
          getsockopt(ls, SOL_SOCKET, SO_TYPE, &type, &tl) == 0 && type == SOCK_STREAM);

    /* ---- connection refused: nothing listens on a fresh port ---- */
    {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = loopback(1);
        errno = 0;
        int r = connect(s, (struct sockaddr *)&a, sizeof(a));
        check("connect to a closed port fails with ECONNREFUSED", r == -1 && errno == ECONNREFUSED);
        close(s);
    }

    /* ---- echo server in a child; the parent streams BIG bytes through it ---- */
    pid_t srv = fork();
    if (srv == 0) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) _exit(10);
        static unsigned char buf[16384];
        ssize_t r;
        while ((r = read(c, buf, sizeof(buf))) > 0)
            if (write_all(c, buf, (size_t)r) != 0) _exit(11);
        shutdown(c, SHUT_WR);
        close(c);
        _exit(r == 0 ? 0 : 12);
    }
    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = loopback(port);
    check("connect to the listener", connect(c, (struct sockaddr *)&a, sizeof(a)) == 0);
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct sockaddr_in peer; socklen_t pl = sizeof(peer);
    check("getpeername reports the server", getpeername(c, (struct sockaddr *)&peer, &pl) == 0
          && ntohs(peer.sin_port) == port && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    check("getsockname gives the client an ephemeral port", bound_port(c) != 0 && bound_port(c) != port);

    unsigned char *out = malloc(BIG), *in = malloc(BIG);
    for (size_t i = 0; i < BIG; i++) out[i] = pat(i);
    /* Writer and reader at once, or the echo's receive window and ours would deadlock. */
    pid_t wr = fork();
    if (wr == 0) {
        _exit(write_all(c, out, BIG) == 0 ? 0 : 1);
    }
    int got = read_all(c, in, BIG);
    int wst = 0;
    waitpid(wr, &wst, 0);
    check("512 KiB written through the echo server", WIFEXITED(wst) && WEXITSTATUS(wst) == 0);
    check("512 KiB echoed back, byte for byte", got == 0 && memcmp(in, out, BIG) == 0);

    /* half-close: the server sees EOF, finishes, and we read EOF in turn */
    check("shutdown(SHUT_WR) succeeds", shutdown(c, SHUT_WR) == 0);
    char tail;
    ssize_t r = read(c, &tail, 1);
    check("the server's close shows up as EOF (read returns 0)", r == 0);
    int st = 0;
    waitpid(srv, &st, 0);
    check("the echo server saw our EOF and exited cleanly", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(c);

    /* ---- poll + accept + non-blocking connect ---- */
    {
        struct pollfd pf = { ls, POLLIN, 0 };
        check("poll: nothing waiting on the listener", poll(&pf, 1, 0) == 0);
        int nb = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        struct sockaddr_in la = loopback(port);
        int cr = connect(nb, (struct sockaddr *)&la, sizeof(la));
        check("non-blocking connect returns EINPROGRESS (or completes)",
              cr == 0 || (cr == -1 && errno == EINPROGRESS));
        pf.revents = 0;
        check("poll: a pending connection makes the listener readable", poll(&pf, 1, 2000) == 1
              && (pf.revents & POLLIN));
        struct pollfd cf = { nb, POLLOUT, 0 };
        check("poll: the connecting socket becomes writable", poll(&cf, 1, 2000) == 1
              && (cf.revents & POLLOUT));
        int err = -1; socklen_t el = sizeof(err);
        getsockopt(nb, SOL_SOCKET, SO_ERROR, &err, &el);
        check("SO_ERROR is 0 after the connect completed", err == 0);
        int s2 = accept(ls, NULL, NULL);
        check("accept returns the connection", s2 >= 0);
        char b[4];
        errno = 0;
        check("non-blocking read with no data gives EAGAIN", read(nb, b, 4) == -1 && errno == EAGAIN);
        write_all(s2, "ping", 4);
        struct pollfd rf = { nb, POLLIN, 0 };
        check("poll: data makes the socket readable", poll(&rf, 1, 2000) == 1 && (rf.revents & POLLIN));
        int avail = 0;
        ioctl(nb, FIONREAD, &avail);
        check("FIONREAD counts the waiting bytes", avail == 4);
        check("read returns the data", read(nb, b, 4) == 4 && memcmp(b, "ping", 4) == 0);
        close(s2);
        struct pollfd hf = { nb, POLLIN, 0 };
        check("poll: the peer's close is readable (EOF)", poll(&hf, 1, 2000) == 1);
        check("and read returns 0", read(nb, b, 4) == 0);
        close(nb);
    }

    /* ---- UDP ---- */
    {
        int u1 = socket(AF_INET, SOCK_DGRAM, 0), u2 = socket(AF_INET, SOCK_DGRAM, 0);
        check("UDP sockets", u1 >= 0 && u2 >= 0);
        struct sockaddr_in ua = loopback(0);
        check("UDP bind", bind(u1, (struct sockaddr *)&ua, sizeof(ua)) == 0);
        unsigned short up = bound_port(u1);
        struct sockaddr_in to = loopback(up);
        check("sendto a datagram",
              sendto(u2, "datagram-1", 10, 0, (struct sockaddr *)&to, sizeof(to)) == 10);
        check("and a second one",
              sendto(u2, "dg-2", 4, 0, (struct sockaddr *)&to, sizeof(to)) == 4);
        char buf[64];
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(u1, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        check("recvfrom returns the first datagram whole", n == 10 && memcmp(buf, "datagram-1", 10) == 0);
        check("the sender's address is reported", fl >= sizeof(from)
              && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && ntohs(from.sin_port) == bound_port(u2));
        n = recvfrom(u1, buf, 2, MSG_TRUNC, NULL, NULL);
        check("MSG_TRUNC reports the real length of a truncated datagram", n == 4);
        errno = 0;
        check("empty queue: non-blocking recv gives EAGAIN",
              recvfrom(u1, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL) == -1 && errno == EAGAIN);
        check("connect() on UDP fixes the peer", connect(u2, (struct sockaddr *)&to, sizeof(to)) == 0);
        check("send() uses it", send(u2, "conn", 4, 0) == 4);
        struct pollfd pf = { u1, POLLIN, 0 };
        check("poll sees the datagram", poll(&pf, 1, 2000) == 1 && (pf.revents & POLLIN));
        check("and read() gets it", read(u1, buf, sizeof(buf)) == 4 && memcmp(buf, "conn", 4) == 0);
        close(u1); close(u2);
    }

    /* ---- many connections in flight, and descriptor limits behave ---- */
    {
        int ok = 1;
        int cl[8], sv[8];
        for (int i = 0; i < 8; i++) {
            cl[i] = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in la = loopback(port);
            if (connect(cl[i], (struct sockaddr *)&la, sizeof(la)) != 0) ok = 0;
        }
        for (int i = 0; i < 8; i++) {
            sv[i] = accept(ls, NULL, NULL);
            if (sv[i] < 0) ok = 0;
        }
        for (int i = 0; i < 8 && ok; i++) {
            char m[8], b[8];
            snprintf(m, sizeof(m), "conn-%d", i);
            write_all(cl[i], m, 6);
            if (read_all(sv[i], b, 6) != 0 || memcmp(b, m, 6) != 0) ok = 0;
        }
        check("eight simultaneous connections each carry their own data", ok);
        for (int i = 0; i < 8; i++) { close(cl[i]); if (sv[i] >= 0) close(sv[i]); }
    }

    /* ---- writing to a closed connection ---- */
    {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in la = loopback(port);
        connect(s, (struct sockaddr *)&la, sizeof(la));
        int p = accept(ls, NULL, NULL);
        close(p);
        usleep(100000);
        errno = 0;
        ssize_t w = -1;
        for (int i = 0; i < 20 && w != -1; i++) w = write(s, "x", 1);   /* first write may succeed */
        for (int i = 0; i < 20; i++) { w = write(s, "x", 1); if (w == -1) break; usleep(20000); }
        check("writing to a connection the peer closed fails (EPIPE/ECONNRESET)",
              w == -1 && (errno == EPIPE || errno == ECONNRESET));
        close(s);
    }

    /* ---- stdio on a non-blocking socket (what BusyBox wget does) ----
     * fread() must return the bytes it had already read when the stream runs dry
     * (a short count with errno EAGAIN), not 0: returning 0 lost whole chunks. */
    {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in la = loopback(port);
        connect(s, (struct sockaddr *)&la, sizeof(la));
        int p = accept(ls, NULL, NULL);
        pid_t w = fork();
        if (w == 0) {
            close(s);
            unsigned char piece[100];
            for (int i = 0; i < 60; i++) {
                for (int k = 0; k < 100; k++) piece[k] = pat((size_t)(i * 100 + k));
                if (write(p, piece, 100) != 100) _exit(1);
                usleep(4000);
            }
            _exit(0);
        }
        close(p);
        fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
        FILE *f = fdopen(s, "r");
        size_t total = 0;
        int good = 1;
        unsigned char b[4096];
        while (total < 6000) {
            errno = 0;
            size_t n = fread(b, 1, sizeof(b), f);
            for (size_t i = 0; i < n; i++)
                if (b[i] != pat(total + i)) good = 0;
            total += n;
            if (n == 0) {
                if (errno != EAGAIN) break;
                clearerr(f);
                struct pollfd pf = { s, POLLIN, 0 };
                poll(&pf, 1, 1000);
            }
        }
        check("fread on a non-blocking socket loses no bytes (6000 trickled)", total == 6000 && good);
        fclose(f);
        waitpid(w, NULL, 0);
    }

    close(ls);
    printf(failures ? "[net_test] %d FAILED\n" : "[net_test] ALL PASSED\n", failures);
    return failures ? 1 : 0;
}
