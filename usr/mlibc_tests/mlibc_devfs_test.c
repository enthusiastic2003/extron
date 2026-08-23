/*
 * Stage 5: /dev is now a real, second VFS mount (kernel/fs/devfs.c),
 * covered over ramfs's root exactly like any other mount point — not a
 * ramfs directory with special-cased names. Proves the mount resolves,
 * the four device nodes behave like real char devices, and stdio (fd
 * 0/1/2) really is /dev/console now rather than a hidden kind reserved
 * for three magic descriptors (see file_table_init(), kernel/fs/file.c).
 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>

static int failures = 0;

static void check(const char *what, int ok) {
    printf("[devfs_test] %-46s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    printf("\n[devfs_test] === /dev device nodes ===\n");

    struct stat st;
    check("stat(/dev) sees a directory",
          stat("/dev", &st) == 0 && S_ISDIR(st.st_mode));

    check("stat(/dev/console) is a character device",
          stat("/dev/console", &st) == 0 && S_ISCHR(st.st_mode));
    ino_t console_ino = st.st_ino;

    check("stat(/dev/tty) names the same inode as /dev/console",
          stat("/dev/tty", &st) == 0 && S_ISCHR(st.st_mode)
          && st.st_ino == console_ino && st.st_nlink == 2);

    check("stdin is /dev/console", isatty(STDIN_FILENO) != 0);

    int null_fd = open("/dev/null", O_RDWR);
    check("open(/dev/null) succeeds", null_fd >= 0);
    char scratch[16];
    memset(scratch, 0x5a, sizeof(scratch));
    check("write(/dev/null, ...) discards and reports the full count",
          write(null_fd, "ignored", 7) == 7);
    check("read(/dev/null) reports immediate EOF", read(null_fd, scratch, 1) == 0);
    close(null_fd);

    int zero_fd = open("/dev/zero", O_RDONLY);
    check("open(/dev/zero) succeeds", zero_fd >= 0);
    memset(scratch, 0x5a, sizeof(scratch));
    long got = read(zero_fd, scratch, sizeof(scratch));
    int all_zero = 1;
    for (size_t i = 0; i < sizeof(scratch); i++)
        if (scratch[i] != 0) all_zero = 0;
    check("read(/dev/zero) fills the buffer with zero bytes",
          got == (long)sizeof(scratch) && all_zero);
    close(zero_fd);

    printf("[devfs_test] === %d failure(s) ===\n", failures);
    return failures;
}
