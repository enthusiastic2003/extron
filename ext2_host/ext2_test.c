/*
 * ext2_test.c — Host test harness for the ext2 driver.
 *
 * Compile with:
 *   gcc -DEXT2_HOST_TEST -Wall -Wextra -O1 -g ext2.c ext2_test.c -o ext2_test
 *
 * Usage:
 *   ./ext2_test ext2.img
 *
 * The ext2.img must be created by make_test_img.sh, which populates it
 * with known test content.
 */

#include "ext2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/*  Test framework (matches the mlibc_tests/ pattern)                 */
/* ------------------------------------------------------------------ */

static int failures = 0;
static int checks   = 0;

#define check(cond, fmt, ...) do {                                      \
    checks++;                                                           \
    if (!(cond)) {                                                      \
        failures++;                                                     \
        fprintf(stderr, "FAIL [%s:%d]: " fmt "\n",                     \
                __FILE__, __LINE__, ##__VA_ARGS__);                    \
    } else {                                                            \
        fprintf(stderr, "  ok: " fmt "\n", ##__VA_ARGS__);             \
    }                                                                   \
} while (0)

/* ------------------------------------------------------------------ */
/*  pread()-backed block device                                       */
/* ------------------------------------------------------------------ */

struct file_block_dev {
    struct ext2_block_dev base;
    int fd;
};

static int file_read_sectors(struct ext2_block_dev *dev,
                             uint64_t lba, size_t count, void *buf)
{
    struct file_block_dev *fbd = (struct file_block_dev *)dev;
    size_t bytes = count * dev->sector_size;
    off_t  off   = (off_t)(lba * dev->sector_size);

    ssize_t n = pread(fbd->fd, buf, bytes, off);
    if (n < 0)
        return -EIO;
    if ((size_t)n < bytes) {
        /* Short read — zero the rest (may happen near end of image). */
        memset((uint8_t *)buf + n, 0, bytes - (size_t)n);
    }
    return 0;
}

static struct file_block_dev *open_image(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return NULL;
    }

    struct file_block_dev *fbd = calloc(1, sizeof(*fbd));
    if (!fbd) {
        close(fd);
        return NULL;
    }

    fbd->fd = fd;
    fbd->base.read_sectors = file_read_sectors;
    fbd->base.sector_size  = 512;
    fbd->base.sector_count = (uint64_t)st.st_size / 512;

    return fbd;
}

static void close_image(struct file_block_dev *fbd)
{
    if (!fbd) return;
    close(fbd->fd);
    free(fbd);
}

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

static void test_mount(struct ext2_mount *m)
{
    check(m != NULL, "ext2_mount_create succeeds");
    if (!m) return;
    check(m->block_size == 1024 || m->block_size == 2048 || m->block_size == 4096,
          "block_size is valid (%u)", m->block_size);
    check(m->total_inodes > 0, "total_inodes > 0 (%u)", m->total_inodes);
    check(m->total_blocks > 0, "total_blocks > 0 (%u)", m->total_blocks);
}

static void test_root_inode(struct ext2_mount *m)
{
    struct ext2_inode_info *root = ext2_lookup_inode(m, EXT2_ROOT_INO);
    check(root != NULL, "root inode (2) lookup succeeds");
    if (!root) return;

    check((root->disk.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR,
          "root inode is a directory (mode=0%o)", root->disk.i_mode);
    check(root->disk.i_links_count >= 2,
          "root inode nlink >= 2 (%u)", root->disk.i_links_count);

    ext2_free_inode_info(root);
}

static void test_read_known_file(struct ext2_mount *m)
{
    struct ext2_inode_info *info = ext2_lookup_path(m, "/hello.txt");
    check(info != NULL, "lookup /hello.txt succeeds");
    if (!info) return;

    check((info->disk.i_mode & EXT2_S_IFMT) == EXT2_S_IFREG,
          "hello.txt is a regular file");

    /* make_test_img.sh writes: echo -n "hello from ext2" */
    const char *expected = "hello from ext2";
    size_t expected_len = strlen(expected);

    check(info->disk.i_size == expected_len,
          "hello.txt size is %zu (got %u)", expected_len, info->disk.i_size);

    char buf[256];
    memset(buf, 0, sizeof(buf));
    long n = ext2_read_data(m, info, 0, buf, sizeof(buf));
    check(n == (long)expected_len,
          "read returned %ld bytes (expected %zu)", n, expected_len);
    check(n > 0 && memcmp(buf, expected, (size_t)n) == 0,
          "hello.txt content matches: \"%.*s\"", (int)n, buf);

    ext2_free_inode_info(info);
}

static void test_nested_file(struct ext2_mount *m)
{
    struct ext2_inode_info *info = ext2_lookup_path(m, "/subdir/nested.txt");
    check(info != NULL, "lookup /subdir/nested.txt succeeds");
    if (!info) return;

    const char *expected = "nested content";
    char buf[256];
    memset(buf, 0, sizeof(buf));
    long n = ext2_read_data(m, info, 0, buf, sizeof(buf));
    check(n == (long)strlen(expected),
          "nested.txt read %ld bytes (expected %zu)", n, strlen(expected));
    check(n > 0 && memcmp(buf, expected, (size_t)n) == 0,
          "nested.txt content matches");

    ext2_free_inode_info(info);
}

static void test_large_file(struct ext2_mount *m)
{
    struct ext2_inode_info *info = ext2_lookup_path(m, "/big.bin");
    check(info != NULL, "lookup /big.bin succeeds");
    if (!info) return;

    /* 52KB file created by make_test_img.sh — forces single-indirect
     * blocks when block_size = 1024 (> 12 direct blocks). */
    uint32_t expected_size = 52 * 1024;
    check(info->disk.i_size == expected_size,
          "big.bin size is %u (expected %u)", info->disk.i_size, expected_size);

    /* Read the whole thing and verify it's not all zeros (since the
     * source was /dev/urandom, all-zeros would indicate a read failure). */
    uint8_t *data = malloc(expected_size);
    if (!data) {
        check(0, "malloc for big.bin read");
        ext2_free_inode_info(info);
        return;
    }

    long n = ext2_read_data(m, info, 0, data, expected_size);
    check(n == (long)expected_size,
          "big.bin read returned %ld (expected %u)", n, expected_size);

    /* Check that the data isn't all zeros. */
    int nonzero = 0;
    for (size_t i = 0; i < expected_size && !nonzero; i++)
        if (data[i] != 0) nonzero = 1;
    check(nonzero, "big.bin contains non-zero data (not a hole)");

    /* Test partial read at an offset. */
    uint8_t partial[128];
    long p = ext2_read_data(m, info, 1000, partial, sizeof(partial));
    check(p == 128, "partial read at offset 1000 returned %ld", p);
    check(memcmp(partial, data + 1000, 128) == 0,
          "partial read content matches full read");

    /* Test read past EOF. */
    long past = ext2_read_data(m, info, expected_size, partial, sizeof(partial));
    check(past == 0, "read at EOF returns 0 (got %ld)", past);

    /* Read that crosses EOF boundary. */
    long cross = ext2_read_data(m, info, expected_size - 10, partial, 128);
    check(cross == 10, "cross-EOF read returns 10 (got %ld)", cross);

    free(data);
    ext2_free_inode_info(info);
}

static void test_readdir(struct ext2_mount *m)
{
    struct ext2_inode_info *root = ext2_lookup_inode(m, EXT2_ROOT_INO);
    check(root != NULL, "root inode for readdir");
    if (!root) return;

    /* Enumerate all entries. */
    int found_hello = 0, found_subdir = 0, found_big = 0;
    int found_link_short = 0, found_link_long = 0;
    int found_dot = 0, found_dotdot = 0;
    int count = 0;

    for (size_t i = 0; ; i++) {
        struct ext2_dir_result res;
        int ret = ext2_iter_dir(m, root, i, &res);
        if (ret == 0) break;  /* past end */
        check(ret == 1, "readdir entry %zu returns 1", i);
        if (ret != 1) break;

        count++;
        if (strcmp(res.name, ".") == 0)           found_dot = 1;
        else if (strcmp(res.name, "..") == 0)     found_dotdot = 1;
        else if (strcmp(res.name, "hello.txt") == 0)   found_hello = 1;
        else if (strcmp(res.name, "subdir") == 0)      found_subdir = 1;
        else if (strcmp(res.name, "big.bin") == 0)      found_big = 1;
        else if (strcmp(res.name, "link_short") == 0)   found_link_short = 1;
        else if (strcmp(res.name, "link_long") == 0)    found_link_long = 1;
    }

    check(found_dot,        "readdir: found '.'");
    check(found_dotdot,     "readdir: found '..'");
    check(found_hello,      "readdir: found 'hello.txt'");
    check(found_subdir,     "readdir: found 'subdir'");
    check(found_big,        "readdir: found 'big.bin'");
    check(found_link_short, "readdir: found 'link_short'");
    check(found_link_long,  "readdir: found 'link_long'");
    check(count >= 7,       "readdir: at least 7 entries (got %d)", count);

    ext2_free_inode_info(root);
}

static void test_readdir_subdir(struct ext2_mount *m)
{
    struct ext2_inode_info *subdir = ext2_lookup_path(m, "/subdir");
    check(subdir != NULL, "lookup /subdir succeeds");
    if (!subdir) return;

    check((subdir->disk.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR,
          "/subdir is a directory");

    int found_nested = 0;
    for (size_t i = 0; ; i++) {
        struct ext2_dir_result res;
        int ret = ext2_iter_dir(m, subdir, i, &res);
        if (ret <= 0) break;
        if (strcmp(res.name, "nested.txt") == 0)
            found_nested = 1;
    }
    check(found_nested, "readdir /subdir: found 'nested.txt'");

    ext2_free_inode_info(subdir);
}

static void test_symlink_short(struct ext2_mount *m)
{
    struct ext2_inode_info *info = ext2_lookup_path(m, "/link_short");
    check(info != NULL, "lookup /link_short succeeds");
    if (!info) return;

    check((info->disk.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK,
          "link_short is a symlink (mode=0%o)", info->disk.i_mode);

    /* Fast-path: target "hello.txt" is < 60 bytes, stored in i_block[]. */
    check(info->disk.i_blocks == 0,
          "link_short uses fast-path (i_blocks=%u)", info->disk.i_blocks);

    char target[256];
    memset(target, 0, sizeof(target));
    long n = ext2_read_symlink(m, info, target, sizeof(target));
    check(n > 0, "readlink returned %ld", n);
    check(strcmp(target, "hello.txt") == 0,
          "link_short target is 'hello.txt' (got '%s')", target);

    ext2_free_inode_info(info);
}

static void test_symlink_with_xattr(struct ext2_mount *m)
{
    /* Regression test: a symlink whose target is short enough for the
     * fast/inline path (< 60 bytes) but which also has a real extended
     * attribute block attached (i_file_acl != 0) — exactly what any
     * file or symlink written by a host tool under SELinux (or
     * anything else that sets an xattr) looks like on disk. i_blocks
     * alone is nonzero here purely because of the xattr block's own
     * sector count, NOT because a real data block was allocated for
     * the target. A fast-path check that only looks at i_blocks == 0
     * misses this, falls through to the slow/block-pointer path, and
     * misreads the raw inline target bytes as if they were indirect
     * block pointers instead. */
    struct ext2_inode_info *info = ext2_lookup_path(m, "/link_with_xattr");
    check(info != NULL, "lookup /link_with_xattr succeeds");
    if (!info) return;

    check((info->disk.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK,
          "link_with_xattr is a symlink");
    check(info->disk.i_file_acl != 0,
          "link_with_xattr has a real xattr block (i_file_acl=%u)",
          info->disk.i_file_acl);
    check(info->disk.i_blocks != 0,
          "link_with_xattr's i_blocks is nonzero purely from the xattr block (i_blocks=%u)",
          info->disk.i_blocks);

    char target[256];
    memset(target, 0, sizeof(target));
    long n = ext2_read_symlink(m, info, target, sizeof(target));
    check(n > 0, "readlink returned %ld", n);
    check(strcmp(target, "hello.txt") == 0,
          "link_with_xattr target is still 'hello.txt' despite the xattr block (got '%s')",
          target);

    ext2_free_inode_info(info);
}

static void test_symlink_long(struct ext2_mount *m)
{
    struct ext2_inode_info *info = ext2_lookup_path(m, "/link_long");
    check(info != NULL, "lookup /link_long succeeds");
    if (!info) return;

    check((info->disk.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK,
          "link_long is a symlink");

    /* Slow path: target is 61 'a's, > 60 bytes. */
    char target[256];
    memset(target, 0, sizeof(target));
    long n = ext2_read_symlink(m, info, target, sizeof(target));
    check(n == 61, "readlink returned %ld (expected 61)", n);

    /* Build expected target. */
    char expected[62];
    memset(expected, 'a', 61);
    expected[61] = '\0';
    check(strcmp(target, expected) == 0,
          "link_long target is 61 'a's");

    ext2_free_inode_info(info);
}

static void test_enoent(struct ext2_mount *m)
{
    struct ext2_inode_info *info = ext2_lookup_path(m, "/nonexistent");
    check(info == NULL, "lookup /nonexistent returns NULL");
    if (info) ext2_free_inode_info(info);

    info = ext2_lookup_path(m, "/subdir/nope");
    check(info == NULL, "lookup /subdir/nope returns NULL");
    if (info) ext2_free_inode_info(info);
}

static void test_eisdir(struct ext2_mount *m)
{
    struct ext2_inode_info *root = ext2_lookup_inode(m, EXT2_ROOT_INO);
    check(root != NULL, "root inode for EISDIR test");
    if (!root) return;

    char buf[64];
    long n = ext2_read_data(m, root, 0, buf, sizeof(buf));
    check(n == -EISDIR, "read on directory returns -EISDIR (got %ld)", n);

    ext2_free_inode_info(root);
}

static void test_enotdir(struct ext2_mount *m)
{
    struct ext2_inode_info *info = ext2_lookup_path(m, "/hello.txt");
    check(info != NULL, "lookup /hello.txt for ENOTDIR test");
    if (!info) return;

    struct ext2_dir_result res;
    int ret = ext2_iter_dir(m, info, 0, &res);
    check(ret == -ENOTDIR, "readdir on file returns -ENOTDIR (got %d)", ret);

    ext2_free_inode_info(info);
}

static void test_path_variations(struct ext2_mount *m)
{
    /* Root lookup. */
    struct ext2_inode_info *info = ext2_lookup_path(m, "/");
    check(info != NULL, "lookup '/' succeeds");
    if (info) {
        check(info->ino == EXT2_ROOT_INO, "'/' is inode 2 (got %u)", info->ino);
        ext2_free_inode_info(info);
    }

    /* Multiple slashes. */
    info = ext2_lookup_path(m, "///hello.txt");
    check(info != NULL, "lookup '///hello.txt' succeeds");
    if (info) ext2_free_inode_info(info);

    /* Trailing slash. */
    info = ext2_lookup_path(m, "/subdir/");
    check(info != NULL, "lookup '/subdir/' succeeds");
    if (info) ext2_free_inode_info(info);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <ext2.img>\n", argv[0]);
        return 1;
    }

    struct file_block_dev *fbd = open_image(argv[1]);
    if (!fbd) return 1;

    struct ext2_mount *m = ext2_mount_create(&fbd->base, 0);
    test_mount(m);
    if (!m) {
        fprintf(stderr, "ext2_mount_create failed — cannot continue\n");
        close_image(fbd);
        return 1;
    }

    test_root_inode(m);
    test_read_known_file(m);
    test_nested_file(m);
    test_large_file(m);
    test_readdir(m);
    test_readdir_subdir(m);
    test_symlink_short(m);
    test_symlink_with_xattr(m);
    test_symlink_long(m);
    test_enoent(m);
    test_eisdir(m);
    test_enotdir(m);
    test_path_variations(m);

    ext2_mount_destroy(m);
    close_image(fbd);

    fprintf(stderr, "\n=== %d check(s), %d failure(s) ===\n", checks, failures);
    return failures > 0 ? 1 : 0;
}
