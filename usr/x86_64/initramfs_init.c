/* /init of the Extron initramfs: find the real root filesystem on the disk named
 * by root= on the kernel command line, make it "/", and run its init.
 *
 *   root=/dev/vda   init=/sbin/init (default)   rootfstype=ext2 (the only one)
 *
 * The kernel has already mounted /proc and /dev over this initramfs. After the
 * switch the new root's own /proc and /dev are mounted. If anything fails the
 * initramfs shell (BusyBox) is started so the system can be repaired. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>

#define SYS_MOUNT       122
#define SYS_SWITCH_ROOT 123

struct mount_request { uint64_t source, target, fstype, flags; };

static long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "memory", "cc");
    return r;
}

static int do_mount(const char *source, const char *target, const char *fstype) {
    struct mount_request req = { (uint64_t)(uintptr_t)source, (uint64_t)(uintptr_t)target,
                                 (uint64_t)(uintptr_t)fstype, 0 };
    return (int)sys3(SYS_MOUNT, (long)&req, 0, 0);
}

/* value of key= on the command line, or NULL */
static const char *cmdline_value(const char *cmdline, const char *key, char *out, size_t size) {
    size_t klen = strlen(key);
    for (const char *p = cmdline; *p;) {
        while (*p == ' ') p++;
        const char *w = p;
        while (*p && *p != ' ') p++;
        if ((size_t)(p - w) > klen && !strncmp(w, key, klen) && w[klen] == '=') {
            size_t n = (size_t)(p - w) - klen - 1;
            if (n >= size) n = size - 1;
            memcpy(out, w + klen + 1, n);
            out[n] = 0;
            return out;
        }
    }
    return NULL;
}

static void emergency(const char *why) {
    printf("\ninit: %s\ninit: starting the initramfs shell\n", why);
    fflush(stdout);
    execl("/bin/sh", "sh", (char *)NULL);
    printf("init: no shell either (%s)\n", strerror(errno));
    for (;;) pause();
}

int main(void) {
    char cmdline[512] = "", root[64] = "", init[64] = "/sbin/init";
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, cmdline, sizeof(cmdline) - 1);
        cmdline[n > 0 ? n : 0] = 0;
        close(fd);
    }
    cmdline_value(cmdline, "init", init, sizeof(init));
    if (!cmdline_value(cmdline, "root", root, sizeof(root)))
        emergency("no root= on the kernel command line");

    if (do_mount(root, "/newroot", "ext2") < 0) {
        char msg[160];
        snprintf(msg, sizeof(msg), "cannot mount %s (ext2) on /newroot", root);
        emergency(msg);
    }
    if (sys3(SYS_SWITCH_ROOT, (long)"/newroot", 0, 0) < 0)
        emergency("switch_root failed");
    if (chdir("/") < 0)
        emergency("chdir / failed");
    /* The new root's pseudo filesystems. Failures are survivable. */
    if (do_mount("proc", "/proc", "proc") < 0)
        printf("init: warning: cannot mount /proc\n");
    if (do_mount("devtmpfs", "/dev", "devtmpfs") < 0)
        printf("init: warning: cannot mount /dev\n");

    char *const argv[] = { init, NULL };
    execv(init, argv);
    printf("init: cannot execute %s: %s\n", init, strerror(errno));
    emergency("the real init did not start");
    return 1;
}
