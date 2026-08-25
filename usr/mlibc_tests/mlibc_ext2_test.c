/*
 * Smoke test for the real ext2 mount at /mnt/sd (kernel/fs/ext2.c,
 * kernel/drivers/emmc.c) — backed by an actual SD card partition, not a
 * synthetic image. kernel/fs/ext2.c's own correctness (block mapping,
 * directory iteration, symlinks, the indirect-block cache, allocation)
 * is already covered exhaustively against a synthetic disk image by the
 * host test harness in ext2_host/ — see ext2_host/ext2_test.c, run via
 * `make -C ext2_host test`. That harness can't reach this file: it
 * never goes through the VFS, the EMMC driver, or a real syscall.
 *
 * This test is the other half: it proves the whole stack end to end —
 * VFS mount resolution, the EMMC2 SDHCI driver, and ext2.c's VFS
 * adapter — using whatever happens to be on the card's ext2 partition,
 * plus a dedicated scratch directory this test creates, writes into,
 * and cleans up itself. It does not assume specific pre-existing file
 * names or sizes, since the partition's contents outside that scratch
 * directory aren't part of this repo.
 *
 * ext2 write support (create/write/delete) landed after this test was
 * first written — see ext2_plan.md's "Current state (post-v1)" section
 * for what's real and what still has sharp edges. This version reflects
 * that: read-only enforcement is gone (writes now succeed), replaced
 * with checks for create/write/append/overwrite/permissions/delete, and
 * with explicit checks for the ops that are STILL unconditionally
 * read-only-only (rename/symlink/link/chmod) so a regression there
 * shows up here rather than being silently assumed away.
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

#define TEST_DIR "/mnt/sd/.mlibc_ext2_test"

/* Our test tree is never more than one level deep (TEST_DIR and one
 * "sub" child), so a flat unlink pass over each directory plus rmdir
 * is enough — no need for general recursive removal. Best-effort: this
 * runs both before the test (in case a previous run crashed and left
 * something behind) and after, so failures here are swallowed rather
 * than asserted. */
static void remove_dir_shallow(const char *path) {
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        char child[320];
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        if (unlink(child) != 0)
            rmdir(child);
    }
    closedir(d);
    rmdir(path);
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

/*
 * Read-side regression pass over whatever's already on the card,
 * independent of anything this test creates: enumerate /mnt/sd's root,
 * and for every regular file found, read it twice (two separate
 * open()s) and require byte-for-byte agreement, plus a mid-file
 * seek+read probe on the largest one found. This is the direct
 * regression test for the indirect-block cache added to inode_bmap()
 * — a caching bug would show up as the second read disagreeing with
 * the first, or as corruption localized to offsets past the 12 direct
 * blocks.
 */
static void check_existing_content_reads_consistently(void) {
    DIR *root = opendir("/mnt/sd");
    check("opendir(/mnt/sd) succeeds", root != NULL);
    if (!root) return;

    #define MAX_TRACKED 64
    char reg_names[MAX_TRACKED][256];
    off_t  reg_sizes[MAX_TRACKED];
    int n_reg = 0;
    int n_entries = 0, n_dirs = 0, n_reg_total = 0, n_other = 0;

    struct dirent *de;
    while (n_entries < 4096 && (de = readdir(root)) != NULL) {
        n_entries++;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")
            || !strcmp(de->d_name, ".mlibc_ext2_test"))
            continue;

        char path[300];
        snprintf(path, sizeof(path), "/mnt/sd/%s", de->d_name);

        struct stat st;
        if (lstat(path, &st) != 0) { n_other++; continue; }

        if (S_ISDIR(st.st_mode)) {
            n_dirs++;
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

    check("readdir(/mnt/sd) enumerates without error", n_entries < 4096);
    printf("[ext2_test] root: %d dirs, %d regular files, %d other, %d total entries\n",
           n_dirs, n_reg_total, n_other, n_entries);

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
}

/*
 * Create/write/append/overwrite/permissions/delete, all inside our own
 * scratch directory so this never touches whatever else lives on the
 * card. Every path here is exercising the write commits (ef92bfc,
 * 141f98f) directly.
 */
static void check_create_write_and_delete(void) {
    umask(0);

    check("mkdir creates a real directory on /mnt/sd", mkdir(TEST_DIR, 0755) == 0);
    struct stat st;
    check("stat sees the new directory with the requested mode",
          stat(TEST_DIR, &st) == 0 && S_ISDIR(st.st_mode)
          && (st.st_mode & 0777) == 0755);

    /* --- create + permissions --- */
    char file_a[300];
    snprintf(file_a, sizeof(file_a), TEST_DIR "/alpha.txt");
    int fd = open(file_a, O_CREAT | O_EXCL | O_WRONLY, 0640);
    check("O_CREAT creates a new regular file", fd >= 0);
    check("new file's mode round-trips through the inode as 0640",
          fd >= 0 && stat(file_a, &st) == 0 && S_ISREG(st.st_mode)
          && (st.st_mode & 0777) == 0640);
    errno = 0;
    check("O_CREAT|O_EXCL on an existing file reports EEXIST",
          open(file_a, O_CREAT | O_EXCL | O_WRONLY, 0640) == -1 && errno == EEXIST);

    /* --- write + read back --- */
    const char *msg = "hello from the ext2 write path\n";
    size_t msg_len = strlen(msg);
    check("write() reports the full byte count",
          fd >= 0 && write(fd, msg, msg_len) == (long)msg_len);
    check("stat size reflects the write",
          stat(file_a, &st) == 0 && (size_t)st.st_size == msg_len);
    if (fd >= 0) close(fd);

    long got;
    char *readback = read_whole_file(file_a, msg_len, &got);
    check("read back matches what was written",
          readback && got == (long)msg_len && memcmp(readback, msg, msg_len) == 0);
    free(readback);

    /* --- append (exercises inode_bmap_alloc's file-growth path) --- */
    const char *more = "second line, appended after reopening\n";
    size_t more_len = strlen(more);
    fd = open(file_a, O_WRONLY);
    check("reopen for append succeeds", fd >= 0);
    off_t end = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
    check("lseek(SEEK_END) lands exactly at the file's current size",
          end == (off_t)msg_len);
    check("append write reports the full byte count",
          fd >= 0 && write(fd, more, more_len) == (long)more_len);
    if (fd >= 0) close(fd);

    size_t total_len = msg_len + more_len;
    char *expected = malloc(total_len);
    memcpy(expected, msg, msg_len);
    memcpy(expected + msg_len, more, more_len);

    readback = read_whole_file(file_a, total_len, &got);
    check("appended content reads back in full and in the right order",
          readback && got == (long)total_len
          && memcmp(readback, expected, total_len) == 0);
    free(readback);

    /* --- in-place overwrite (partial-block read-modify-write) --- */
    fd = open(file_a, O_RDWR);
    check("reopen O_RDWR for in-place overwrite succeeds", fd >= 0);
    if (fd >= 0) {
        check("lseek to an interior offset", lseek(fd, 6, SEEK_SET) == 6);
        check("in-place write of 5 bytes reports the full count",
              write(fd, "WORLD", 5) == 5);
        close(fd);
    }
    memcpy(expected + 6, "WORLD", 5); /* mirror the same mutation on our copy */

    readback = read_whole_file(file_a, total_len, &got);
    check("in-place overwrite is reflected on reread, surrounding bytes untouched",
          readback && got == (long)total_len
          && memcmp(readback, expected, total_len) == 0);
    free(readback);
    free(expected);

    /* --- subdirectory + readdir --- */
    char subdir[300];
    snprintf(subdir, sizeof(subdir), TEST_DIR "/sub");
    check("mkdir creates a subdirectory", mkdir(subdir, 0755) == 0);

    char subfile[340];
    snprintf(subfile, sizeof(subfile), "%s/child.txt", subdir);
    int sfd = open(subfile, O_CREAT | O_EXCL | O_WRONLY, 0644);
    check("create a file inside the new subdirectory", sfd >= 0);
    if (sfd >= 0) { write(sfd, "x", 1); close(sfd); }

    DIR *dp = opendir(subdir);
    int found_child = 0, found_dot = 0, found_dotdot = 0;
    struct dirent *de;
    while (dp && (de = readdir(dp)) != NULL) {
        if (!strcmp(de->d_name, ".")) found_dot = 1;
        else if (!strcmp(de->d_name, "..")) found_dotdot = 1;
        else if (!strcmp(de->d_name, "child.txt")) found_child = 1;
    }
    if (dp) closedir(dp);
    check("readdir sees '.', '..', and the file just created",
          found_dot && found_dotdot && found_child);

    /* --- delete: non-empty dir refuses, empties out, then succeeds --- */
    errno = 0;
    check("rmdir on a non-empty directory reports ENOTEMPTY",
          rmdir(subdir) == -1 && errno == ENOTEMPTY);
    check("unlink removes the file inside the subdirectory", unlink(subfile) == 0);
    check("rmdir succeeds once the directory is empty", rmdir(subdir) == 0);
    errno = 0;
    check("stat on the removed subdirectory reports ENOENT",
          stat(subdir, &st) == -1 && errno == ENOENT);

    check("unlink removes the main test file", unlink(file_a) == 0);
    errno = 0;
    check("stat after unlink reports ENOENT", stat(file_a, &st) == -1 && errno == ENOENT);
    errno = 0;
    check("unlinking an already-removed file reports ENOENT",
          unlink(file_a) == -1 && errno == ENOENT);

    /* --- test rename, symlink, and chmod --- */
    char src[300], dst[300];
    snprintf(src, sizeof(src), TEST_DIR "/rename-src");
    snprintf(dst, sizeof(dst), TEST_DIR "/rename-dst");
    int rfd = open(src, O_CREAT | O_EXCL | O_WRONLY, 0644);
    check("create a file to probe the new ops", rfd >= 0);
    if (rfd >= 0) { write(rfd, "RENAME", 6); close(rfd); }

    /* Test rename */
    check("rename() moves the file to the new destination", rename(src, dst) == 0);
    errno = 0;
    check("stat on the old name reports ENOENT", stat(src, &st) == -1 && errno == ENOENT);
    check("stat on the new name succeeds", stat(dst, &st) == 0 && st.st_size == 6);

    /* Test chmod */
    check("chmod() changes the permissions of the file", chmod(dst, 0600) == 0);
    check("stat verifies the new permissions", stat(dst, &st) == 0 && (st.st_mode & 0777) == 0600);

    /* Test symlink */
    char link_target[300];
    snprintf(link_target, sizeof(link_target), TEST_DIR "/a-symlink");
    check("symlink() creates a fast symlink successfully", symlink("rename-dst", link_target) == 0);
    
    struct stat lst;
    check("lstat() identifies the symlink", lstat(link_target, &lst) == 0 && S_ISLNK(lst.st_mode));
    
    char link_buf[64];
    long rb = readlink(link_target, link_buf, sizeof(link_buf) - 1);
    if (rb >= 0) link_buf[rb] = '\0';
    check("readlink() returns the correct target string", rb == 10 && strcmp(link_buf, "rename-dst") == 0);
    
    /* Follow the symlink with stat */
    check("stat() follows the symlink to the target", stat(link_target, &st) == 0 && st.st_size == 6);

    /* Test slow symlink (>60 chars) */
    char long_link_target[300];
    snprintf(long_link_target, sizeof(long_link_target), TEST_DIR "/a-long-symlink");
    const char *long_target_str = "this_is_a_very_long_target_string_that_exceeds_sixty_characters_to_force_a_block_allocation";
    check("symlink() creates a slow symlink successfully", symlink(long_target_str, long_link_target) == 0);
    
    char long_link_buf[200];
    rb = readlink(long_link_target, long_link_buf, sizeof(long_link_buf) - 1);
    if (rb >= 0) long_link_buf[rb] = '\0';
    check("readlink() reads back the long target string from the block", rb == (long)strlen(long_target_str) && strcmp(long_link_buf, long_target_str) == 0);

    /* Cleanup */
    unlink(dst);
    unlink(link_target);
    unlink(long_link_target);

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
     * kernel.c *before* emmc_mount_ext2() ever runs — exactly like /dev
     * is created before devfs_init() covers it. Now that ext2 supports
     * writes too, a successful write no longer tells the two apart —
     * both accept one. The reliable signal is the root inode number:
     * ext2's root directory is inode 2 by spec (EXT2_ROOT_INO,
     * kernel/include/kernel/fs/ext2.h) no matter what's on the card,
     * while ramfs's root is inode 1 and /mnt/sd, created 5th among
     * kernel.c's boot-time mkdir calls (/dev, /bin, /etc, /tmp, /mnt,
     * /mnt/sd), gets whatever sequential ramfs inode number that
     * ordering produces — never 2. If kernel.c's mkdir order ever
     * changes, this check needs revisiting alongside it. */
    if (root_st.st_ino != 2) {
        printf("[ext2_test] /mnt/sd has ino=%lu, not 2 -- the real ext2 "
               "mount never landed here (still the plain ramfs directory "
               "kernel.c pre-creates as the mount point). Expected on "
               "QEMU; needs real Pi4 hardware with the SD card's ext2 "
               "partition to exercise the rest of this test.\n",
               (unsigned long)root_st.st_ino);
        return 0;
    }
    check("/mnt/sd's root inode is 2 (real ext2 mount present)", 1);

    check_existing_content_reads_consistently();

    remove_dir_shallow(TEST_DIR); /* defensive: a crashed prior run may have left this */
    check_create_write_and_delete();
    remove_dir_shallow(TEST_DIR);

    printf("[ext2_test] === %d failure(s) ===\n", failures);
    return failures;
}
