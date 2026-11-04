/*
 * Display ownership (kernel/drivers/fbown.c): mapping /dev/fb0 takes the display
 * from the kernel console; only one live process may hold it; exit and munmap()
 * give it back.
 */
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "include/extron/fb.h"

static int failures;

static void check(const char *what, int ok) {
    printf("[fbown_test] %s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* Map the display; returns the pointer or MAP_FAILED (errno set). */
static void *map_fb(size_t *size) {
    int fd = open(EXTRON_FB_PATH, O_RDWR);
    if (fd < 0) return MAP_FAILED;
    struct extron_fb_geometry geo;
    if (read(fd, &geo, sizeof(geo)) != (ssize_t)sizeof(geo) || !geo.size) {
        close(fd);
        errno = ENODEV;
        return MAP_FAILED;
    }
    *size = geo.size;
    void *p = mmap(NULL, geo.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int e = errno;
    close(fd);
    errno = e;
    return p;
}

int main(void) {
    size_t size;
    void *first = map_fb(&size);
    if (first == MAP_FAILED && (errno == ENODEV || errno == ENOENT)) {
        printf("[fbown_test] SKIP: no framebuffer\n");
        printf("[fbown_test] ALL PASSED\n");
        return 0;
    }
    check("mapping /dev/fb0 succeeds", first != MAP_FAILED);
    if (first == MAP_FAILED) return 1;

    /* A second process cannot take the display while we hold it. */
    pid_t c = fork();
    if (c == 0) {
        size_t s2;
        void *p = map_fb(&s2);
        _exit(p == MAP_FAILED && errno == EBUSY ? 0 : 1);
    }
    int st = 0;
    waitpid(c, &st, 0);
    check("a second process gets EBUSY while the display is held",
          WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* The owner itself may map it again (same process). */
    size_t s3;
    void *again = map_fb(&s3);
    check("the owner can map it again", again != MAP_FAILED);
    if (again != MAP_FAILED) munmap(again, s3);

    /* munmap() of every mapping releases it... */
    munmap(first, size);
    c = fork();
    if (c == 0) {
        size_t s2;
        void *p = map_fb(&s2);
        _exit(p != MAP_FAILED ? 0 : 1);
    }
    waitpid(c, &st, 0);
    check("after munmap another process can take the display",
          WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* ...and so does exit: the child above held it and is gone, so we can too. */
    size_t s4;
    void *p = map_fb(&s4);
    check("after the previous owner exited the display is free again", p != MAP_FAILED);
    if (p != MAP_FAILED) munmap(p, s4);

    /* A killed owner releases it as well. */
    int ready[2];
    pipe(ready);
    c = fork();
    if (c == 0) {
        size_t s2;
        void *q = map_fb(&s2);
        char ok = q != MAP_FAILED;
        write(ready[1], &ok, 1);
        pause();
        _exit(0);
    }
    char ok = 0;
    read(ready[0], &ok, 1);
    check("a child takes the display", ok == 1);
    size_t s5;
    void *busy = map_fb(&s5);
    check("EBUSY while the child holds it", busy == MAP_FAILED && errno == EBUSY);
    kill(c, 9);
    waitpid(c, &st, 0);
    void *freed = map_fb(&s5);
    check("a killed owner releases the display", freed != MAP_FAILED);

    printf(failures ? "[fbown_test] %d FAILED\n" : "[fbown_test] ALL PASSED\n", failures);
    return failures ? 1 : 0;
}
