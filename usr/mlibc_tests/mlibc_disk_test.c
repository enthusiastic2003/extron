/*
 * Filesystem workout on the persistent data disk mounted at /mnt (virtio-blk
 * + ext2). Skips cleanly when no disk is attached. Leaves a small tree behind
 * (/mnt/dt) so the host can run e2fsck on the image afterwards.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

static int failures;
static void check(const char *what, int ok) {
    printf("[disk_test] %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static unsigned char pat(size_t i) { return (unsigned char)((i * 31 + (i >> 8) * 7) & 0xFF); }

static int write_pattern(int fd, size_t n) {
    unsigned char buf[4096];
    for (size_t off = 0; off < n; off += sizeof buf) {
        size_t c = n - off < sizeof buf ? n - off : sizeof buf;
        for (size_t i = 0; i < c; i++) buf[i] = pat(off + i);
        if (write(fd, buf, c) != (ssize_t)c) return -1;
    }
    return 0;
}

static int verify_pattern(int fd, size_t n) {
    unsigned char buf[4096];
    if (lseek(fd, 0, SEEK_SET) != 0) return -1;
    for (size_t off = 0; off < n; off += sizeof buf) {
        size_t c = n - off < sizeof buf ? n - off : sizeof buf;
        if (read(fd, buf, c) != (ssize_t)c) return -1;
        for (size_t i = 0; i < c; i++)
            if (buf[i] != pat(off + i)) return -1;
    }
    return 0;
}

/* Remove a tree left behind by an earlier run, so the test is repeatable. */
static void rmtree(const char *path) {
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[512];
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        struct stat cs;
        if (lstat(child, &cs) == 0 && S_ISDIR(cs.st_mode)) rmtree(child);
        else unlink(child);
        rewinddir(d);
    }
    closedir(d);
    rmdir(path);
}

static off_t size_of(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? st.st_size : -1;
}

int main(void) {
    struct stat st;
    setvbuf(stdout, NULL, _IONBF, 0);   /* progress stays visible if the run wedges */
    if (stat("/lost+found", &st) != 0) {
        printf("[disk_test] SKIP: no ext2 disk with lost+found\n");
        return 0;
    }

    rmtree("/mnt/dt");
    mkdir("/mnt/dt", 0755);
    check("mkdir /mnt/dt", stat("/mnt/dt", &st) == 0 && S_ISDIR(st.st_mode));

    /* ---- big file: crosses into double-indirect blocks (1 KiB blocks) ---- */
    const size_t BIG = 400 * 1024;
    int fd = open("/mnt/dt/big", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check("create 400 KiB file", fd >= 0 && write_pattern(fd, BIG) == 0);
    check("size is 400 KiB", size_of("/mnt/dt/big") == (off_t)BIG);
    check("read back matches", verify_pattern(fd, BIG) == 0);
    check("fsync", fsync(fd) == 0);

    /* ---- shrink through every level, verifying survivors each time ---- */
    check("shrink to 150 KiB (frees double-indirect)",
          ftruncate(fd, 150 * 1024) == 0 && size_of("/mnt/dt/big") == 150 * 1024);
    check("prefix intact after shrink", verify_pattern(fd, 150 * 1024) == 0);
    check("shrink to 10 KiB + 123 (frees single-indirect)",
          ftruncate(fd, 10 * 1024 + 123) == 0);
    check("prefix intact", verify_pattern(fd, 10 * 1024 + 123) == 0);
    check("grow back to 20 KiB is zero-filled past the cut", ({
        unsigned char b[64]; int ok = ftruncate(fd, 20 * 1024) == 0
            && pread(fd, b, 64, 10 * 1024 + 123) == 64;
        for (int i = 0; ok && i < 64; i++) ok = b[i] == 0;
        ok; }));
    check("regrow to 400 KiB rewrite + verify", ({
        lseek(fd, 0, SEEK_SET); ftruncate(fd, 0);
        write_pattern(fd, BIG) == 0 && verify_pattern(fd, BIG) == 0; }));
    check("shrink to 0", ftruncate(fd, 0) == 0 && size_of("/mnt/dt/big") == 0);
    close(fd);
    unlink("/mnt/dt/big");
    check("unlink big", access("/mnt/dt/big", F_OK) != 0);

    /* ---- pread/pwrite + sparse ---- */
    fd = open("/mnt/dt/sparse", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check("pwrite far offset (sparse)", pwrite(fd, "END", 3, 300 * 1024) == 3);
    char b[8] = {1,1,1,1,1,1,1,1};
    check("hole reads as zeros", pread(fd, b, 8, 1000) == 8 && !b[0] && !b[7]);
    check("data at far offset", pread(fd, b, 3, 300 * 1024) == 3 && !memcmp(b, "END", 3));
    close(fd);

    /* ---- names: rename, link, symlink, nested dirs ---- */
    fd = open("/mnt/dt/a.txt", O_WRONLY | O_CREAT, 0644);
    write(fd, "hello", 5); close(fd);
    check("rename", rename("/mnt/dt/a.txt", "/mnt/dt/b.txt") == 0
          && access("/mnt/dt/a.txt", F_OK) != 0 && size_of("/mnt/dt/b.txt") == 5);
    check("hard link", link("/mnt/dt/b.txt", "/mnt/dt/c.txt") == 0
          && stat("/mnt/dt/b.txt", &st) == 0 && st.st_nlink == 2);
    check("symlink", symlink("b.txt", "/mnt/dt/s.lnk") == 0);
    char tgt[32] = {0};
    check("readlink", readlink("/mnt/dt/s.lnk", tgt, sizeof tgt - 1) == 5
          && !strcmp(tgt, "b.txt"));
    check("mkdir nested", mkdir("/mnt/dt/d1", 0755) == 0 && mkdir("/mnt/dt/d1/d2", 0755) == 0);
    check("rmdir", mkdir("/mnt/dt/tmpd", 0755) == 0 && rmdir("/mnt/dt/tmpd") == 0);
    errno = 0;
    check("rmdir non-empty -> ENOTEMPTY",
          rmdir("/mnt/dt/d1") == -1 && errno == ENOTEMPTY);

    /* ---- readdir reports correct types ---- */
    int dirs = 0, regs = 0, lnks = 0;
    DIR *d = opendir("/mnt/dt");
    for (struct dirent *e; d && (e = readdir(d)); ) {
        if (!strcmp(e->d_name, "d1") && e->d_type == DT_DIR) dirs++;
        if (!strcmp(e->d_name, "b.txt") && e->d_type == DT_REG) regs++;
        if (!strcmp(e->d_name, "s.lnk") && e->d_type == DT_LNK) lnks++;
    }
    if (d) closedir(d);
    check("readdir d_type: dir / file / symlink", dirs == 1 && regs == 1 && lnks == 1);

    /* ---- many small files: directory growth + inode/block reuse ---- */
    mkdir("/mnt/dt/many", 0755);
    char path[64];
    int made = 0;
    for (int i = 0; i < 300; i++) {
        snprintf(path, sizeof path, "/mnt/dt/many/file_with_a_longish_name_%04d", i);
        fd = open(path, O_WRONLY | O_CREAT, 0644);
        if (fd < 0) break;
        char line[32]; int n = snprintf(line, sizeof line, "payload %d\n", i);
        if (write(fd, line, n) != n) { close(fd); break; }
        close(fd); made++;
    }
    check("create 300 files", made == 300);
    int removed = 0;
    for (int i = 0; i < 300; i += 2) {
        snprintf(path, sizeof path, "/mnt/dt/many/file_with_a_longish_name_%04d", i);
        if (unlink(path) == 0) removed++;
    }
    check("unlink every other one", removed == 150);
    int count = 0;
    d = opendir("/mnt/dt/many");
    for (struct dirent *e; d && (e = readdir(d)); )
        if (strncmp(e->d_name, ".", 1)) count++;
    if (d) closedir(d);
    check("150 entries remain", count == 150);
    snprintf(path, sizeof path, "/mnt/dt/many/file_with_a_longish_name_%04d", 299);
    char got[32] = {0};
    fd = open(path, O_RDONLY);
    check("survivor contents", fd >= 0 && read(fd, got, 31) > 0
          && !strncmp(got, "payload 299", 11));
    if (fd >= 0) close(fd);

    /* leave one shrunk big file behind for the host-side fsck */
    fd = open("/mnt/dt/keep", O_RDWR | O_CREAT | O_TRUNC, 0644);
    write_pattern(fd, 300 * 1024);
    ftruncate(fd, 77 * 1024 + 5);
    check("keep file intact", verify_pattern(fd, 77 * 1024 + 5) == 0);
    fsync(fd); close(fd);

    printf("[disk_test] %s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
