/*
 * Smoke test for the real ext2 mount at /mnt/sd (kernel/fs/ext2.c,
 * kernel/drivers/emmc.c) — read-only, backed by an actual SD card
 * partition, not a synthetic image. kernel/fs/ext2.c's own correctness
 * (block mapping, directory iteration, symlinks, the indirect-block
 * cache) is already covered exhaustively against a synthetic disk image
 * by the host test harness in ext2_host/ — see ext2_host/ext2_test.c,
 * run via `make -C ext2_host test`. That harness can't reach this file:
 * it never goes through the VFS, the EMMC driver, or a real syscall.
 *
 * This test is the other half: it proves the whole stack end to end —
 * VFS mount resolution, the EMMC2 SDHCI driver (command sequencing,
 * the post-select clock speed-up, CMD18 multi-block reads), and
 * ext2.c's VFS adapter — using whatever happens to be on the card's
 * ext2 partition. It does not assume specific file names or sizes,
 * since that partition's contents aren't part of this repo.
 *
 * Real-hardware only: QEMU's raspi4b machine has no working EMMC2
 * model (CMD8 times out during emmc_init()), so /mnt/sd never mounts
 * there. That's a QEMU fidelity gap, not a kernel bug — see
 * ext2_plan.md's "Why this is the hard one" section. When the mount
 * isn't present this test says so and exits 0 rather than failing
 * every subsequent check for an environment it can't do anything
 * about.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int failures = 0;

static void check(const char *what, int ok) {
    printf("[ext2_test] %-56s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* Read up to `size` bytes of `path` into a freshly malloc'd buffer.
 * Loops on short reads — vfs_read()'s current implementations always
 * return the full request short of EOF, but POSIX read() doesn't
 * promise that in general, and this is meant to keep working even if
 * that changes. Returns NULL on any failure. */
static char *read_whole_file(const char *path, size_t size, long *out_got) {
    if (size == 0) {
        *out_got = 0;
        return malloc(1); /* non-NULL sentinel for the empty-file case */
    }
    char *buf = malloc(size);
    if (!buf)
        return NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        free(buf);
        return NULL;
    }
    size_t total = 0;
    while (total < size) {
        long got = read(fd, buf + total, size - total);
        if (got < 0) {
            close(fd);
            free(buf);
            return NULL;
        }
        if (got == 0)
            break; /* EOF before st_size bytes — file shrank underneath us */
        total += (size_t)got;
    }
    close(fd);
    *out_got = (long)total;
    return buf;
}

int main(void) {
    printf("\n[ext2_test] === /mnt/sd (real SD card ext2 partition) ===\n");

    struct stat root_st;
    int root_rc = stat("/mnt/sd", &root_st);
    if (root_rc != 0 || !S_ISDIR(root_st.st_mode)) {
        printf("[ext2_test] /mnt/sd is not mounted (stat rc=%d errno=%d) -- "
               "skipping. Expected on QEMU (no working EMMC2 model); needs "
               "real Pi4 hardware with a partitioned, ext2-formatted SD "
               "card to actually exercise this.\n", root_rc, errno);
        return 0;
    }
    check("stat(/mnt/sd) sees a directory", 1);

    /* /mnt/sd is created as a plain, writable ramfs directory in
     * kernel.c *before* emmc_mount_ext2() ever runs — exactly like
     * /dev is created before devfs_init() covers it (kernel/kernel.c).
     * So the stat() above proves nothing about whether the ext2 mount
     * actually landed here: if EMMC init or the ext2 mount failed (as
     * it always will on QEMU), /mnt/sd is just that empty ramfs
     * directory, still a perfectly good directory, but writable and
     * with none of the card's files in it. The only reliable way to
     * tell the two apart from userspace is to try to write to it:
     * ext2_fs_ops's create hook (kernel/fs/ext2.c) unconditionally
     * returns -EROFS, while ramfs happily creates the file. */
    const char *probe_path = "/mnt/sd/__ext2_test_mount_probe__";
    errno = 0;
    int probe_fd = open(probe_path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (probe_fd >= 0) {
        close(probe_fd);
        unlink(probe_path);
        printf("[ext2_test] /mnt/sd accepted a file creation -- the real "
               "ext2 mount never landed here (still the plain ramfs "
               "directory kernel.c pre-creates as the mount point). "
               "Expected on QEMU; needs real Pi4 hardware with the SD "
               "card's ext2 partition to exercise the rest of this test.\n");
        return 0;
    }
    check("/mnt/sd rejects file creation with EROFS (real ext2 mount present)",
          errno == EROFS);

    DIR *root = opendir("/mnt/sd");
    check("opendir(/mnt/sd) succeeds", root != NULL);
    if (!root) {
        printf("[ext2_test] === %d failure(s) ===\n", failures);
        return failures;
    }

    /* Walk the root directory once, categorizing entries. Bounded so a
     * corrupt or pathological directory can't spin forever; ordinary
     * directories are nowhere near this many entries. */
    #define MAX_SCAN 4096
    #define MAX_TRACKED 64
    char reg_names[MAX_TRACKED][256];
    off_t  reg_sizes[MAX_TRACKED];
    int n_reg = 0;
    char first_dir[256] = {0};
    int have_dir = 0;
    int n_entries = 0, n_dirs = 0, n_reg_total = 0, n_other = 0;

    struct dirent *de;
    while (n_entries < MAX_SCAN && (de = readdir(root)) != NULL) {
        n_entries++;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        char path[300];
        snprintf(path, sizeof(path), "/mnt/sd/%s", de->d_name);

        struct stat st;
        if (lstat(path, &st) != 0) {
            n_other++;
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            n_dirs++;
            if (!have_dir) {
                strncpy(first_dir, path, sizeof(first_dir) - 1);
                have_dir = 1;
            }
        } else if (S_ISREG(st.st_mode)) {
            n_reg_total++;
            if (n_reg < MAX_TRACKED) {
                strncpy(reg_names[n_reg], path, sizeof(reg_names[n_reg]) - 1);
                reg_names[n_reg][sizeof(reg_names[n_reg]) - 1] = '\0';
                reg_sizes[n_reg] = st.st_size;
                n_reg++;
            }
        } else {
            n_other++;
        }
    }
    closedir(root);

    check("readdir(/mnt/sd) enumerates without error", n_entries < MAX_SCAN);
    printf("[ext2_test] root: %d dirs, %d regular files, %d other, %d total entries\n",
           n_dirs, n_reg_total, n_other, n_entries);

    /* Content-consistency check: read every tracked regular file twice
     * (two separate open()s) and require byte-for-byte agreement. This
     * is the direct regression test for the indirect-block cache added
     * to inode_bmap() — a caching bug would show up as the second read
     * disagreeing with the first, or as corruption localized to file
     * offsets past the 12 direct blocks. */
    off_t largest_size = -1;
    char largest_path[300] = {0};
    for (int i = 0; i < n_reg; i++) {
        long got_a = 0, got_b = 0;
        char *a = read_whole_file(reg_names[i], (size_t)reg_sizes[i], &got_a);
        char *b = read_whole_file(reg_names[i], (size_t)reg_sizes[i], &got_b);

        char label[320];
        snprintf(label, sizeof(label), "read '%s' (%ld bytes) fully",
                 reg_names[i], (long)reg_sizes[i]);
        check(label, a != NULL && got_a == (long)reg_sizes[i]);

        snprintf(label, sizeof(label), "repeated read of '%s' matches", reg_names[i]);
        check(label, a && b && got_a == got_b
                     && (reg_sizes[i] == 0 || memcmp(a, b, (size_t)got_a) == 0));

        if (reg_sizes[i] > largest_size) {
            largest_size = reg_sizes[i];
            strncpy(largest_path, reg_names[i], sizeof(largest_path) - 1);
        }

        free(a);
        free(b);
    }

    /* Targeted random-offset check on whichever file is biggest: seek
     * to its midpoint and confirm a fresh read there agrees with the
     * full-file read above. Only meaningful (as a check on indirect
     * block mapping specifically, rather than just direct blocks) once
     * the file is bigger than a handful of direct blocks; smaller files
     * still get exercised by the full-read check above. */
    if (largest_size > 65536) {
        long got_full = 0;
        char *full = read_whole_file(largest_path, (size_t)largest_size, &got_full);
        if (full && got_full == largest_size) {
            off_t mid = largest_size / 2;
            size_t chunk = 128;
            if ((off_t)chunk > largest_size - mid)
                chunk = (size_t)(largest_size - mid);

            int fd = open(largest_path, O_RDONLY);
            char probe[128];
            long got_probe = -1;
            if (fd >= 0) {
                got_probe = (lseek(fd, mid, SEEK_SET) == mid)
                          ? read(fd, probe, chunk) : -1;
                close(fd);
            }

            char label[320];
            snprintf(label, sizeof(label),
                     "mid-file seek+read of '%s' at offset %ld matches full read",
                     largest_path, (long)mid);
            check(label, fd >= 0 && got_probe == (long)chunk
                         && memcmp(probe, full + mid, chunk) == 0);
        }
        free(full);
    } else {
        printf("[ext2_test] no file over 64KB found on /mnt/sd -- skipping "
               "the mid-file indirect-block probe (largest seen: %ld bytes)\n",
               (long)largest_size);
    }

    /* Further read-only enforcement (O_CREAT was already proven above,
     * as the mount-detection probe itself). ext2_fs_ops's other
     * mutating hooks (remove, rename, link, symlink — kernel/fs/ext2.c)
     * all return -EROFS unconditionally too. A per-node write()
     * attempt is different: ext2_node_ops.write is simply NULL, so
     * vfs_write() reports -EINVAL (kernel/fs/vfs.c's vfs_write())
     * rather than -EROFS — that's the real current behavior, not a
     * documented POSIX guarantee, so this checks what the kernel
     * actually does. */
    errno = 0;
    check("mkdir under /mnt/sd reports EROFS",
          mkdir("/mnt/sd/__ext2_test_dir__", 0755) == -1 && errno == EROFS);

    errno = 0;
    check("symlink under /mnt/sd reports EROFS",
          symlink("whatever", "/mnt/sd/__ext2_test_link__") == -1 && errno == EROFS);

    if (n_reg > 0) {
        errno = 0;
        char label[320];
        snprintf(label, sizeof(label), "unlink('%s') reports EROFS", reg_names[0]);
        check(label, unlink(reg_names[0]) == -1 && errno == EROFS);

        errno = 0;
        int wfd = open(reg_names[0], O_WRONLY);
        long wrote = (wfd >= 0) ? write(wfd, "x", 1) : -1;
        int write_errno = errno;
        if (wfd >= 0) close(wfd);
        snprintf(label, sizeof(label), "write('%s') reports EINVAL", reg_names[0]);
        check(label, wrote == -1 && write_errno == EINVAL);
    } else {
        printf("[ext2_test] no regular file found at /mnt/sd's root -- "
               "skipping unlink/write enforcement checks\n");
    }

    if (have_dir) {
        errno = 0;
        char label[320];
        snprintf(label, sizeof(label), "rmdir('%s') reports EROFS", first_dir);
        check(label, rmdir(first_dir) == -1 && errno == EROFS);
    } else {
        printf("[ext2_test] no subdirectory found at /mnt/sd's root -- "
               "skipping rmdir enforcement check\n");
    }

    printf("[ext2_test] === %d failure(s) ===\n", failures);
    return failures;
}
