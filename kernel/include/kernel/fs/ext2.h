#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>
#include <stddef.h>

/* =====================================================================
 *  Block device interface — the only seam between ext2 and hardware.
 *
 *  The host test harness implements this with pread() on a file.
 *  The kernel's EMMC driver will implement it with emmc_read_sectors().
 *  ext2.c depends on nothing else from the block/driver layer.
 * ===================================================================== */

struct ext2_block_dev {
    int (*read_sectors)(struct ext2_block_dev *dev,
                        uint64_t lba, size_t count, void *buf);
    uint64_t sector_count;
    uint32_t sector_size;   /* 512, virtually always */
};

/* =====================================================================
 *  On-disk structures — packed, little-endian, exact ext2 layout.
 *  Field offsets are verified by _Static_assert in ext2.c.
 * ===================================================================== */

#define EXT2_SUPER_MAGIC  0xEF53
#define EXT2_ROOT_INO     2

/* Superblock — lives at byte offset 1024 within the partition.
 * We only define fields through s_inode_size (90 bytes); the full
 * superblock is 1024 bytes but nothing below needs the rest. */
struct __attribute__((packed)) ext2_superblock {
    uint32_t s_inodes_count;        /* 0   */
    uint32_t s_blocks_count;        /* 4   */
    uint32_t s_r_blocks_count;      /* 8   */
    uint32_t s_free_blocks_count;   /* 12  */
    uint32_t s_free_inodes_count;   /* 16  */
    uint32_t s_first_data_block;    /* 20  */
    uint32_t s_log_block_size;      /* 24  block_size = 1024 << this */
    uint32_t s_log_frag_size;       /* 28  */
    uint32_t s_blocks_per_group;    /* 32  */
    uint32_t s_frags_per_group;     /* 36  */
    uint32_t s_inodes_per_group;    /* 40  */
    uint32_t s_mtime;               /* 44  */
    uint32_t s_wtime;               /* 48  */
    uint16_t s_mnt_count;           /* 52  */
    uint16_t s_max_mnt_count;       /* 54  */
    uint16_t s_magic;               /* 56  must be 0xEF53 */
    uint16_t s_state;               /* 58  */
    uint16_t s_errors;              /* 60  */
    uint16_t s_minor_rev_level;     /* 62  */
    uint32_t s_lastcheck;           /* 64  */
    uint32_t s_checkinterval;       /* 68  */
    uint32_t s_creator_os;          /* 72  */
    uint32_t s_rev_level;           /* 76  0=rev0 (inode_size=128) */
    uint16_t s_def_resuid;          /* 80  */
    uint16_t s_def_resgid;          /* 82  */
    /* --- rev >= 1 fields --- */
    uint32_t s_first_ino;           /* 84  */
    uint16_t s_inode_size;          /* 88  128 for rev0 */
};

/* Block group descriptor — 32 bytes each, table immediately follows
 * the superblock's block. */
struct __attribute__((packed)) ext2_bgd {
    uint32_t bg_block_bitmap;       /* 0  */
    uint32_t bg_inode_bitmap;       /* 4  */
    uint32_t bg_inode_table;        /* 8  — this is the one we need */
    uint16_t bg_free_blocks_count;  /* 12 */
    uint16_t bg_free_inodes_count;  /* 14 */
    uint16_t bg_used_dirs_count;    /* 16 */
    uint16_t bg_pad;                /* 18 */
    uint8_t  bg_reserved[12];       /* 20 */
};

/* Inode — s_inode_size bytes on disk (128 for rev0, 256 for rev1).
 * We define the standard 128-byte layout. */
struct __attribute__((packed)) ext2_disk_inode {
    uint16_t i_mode;                /* 0  */
    uint16_t i_uid;                 /* 2  */
    uint32_t i_size;                /* 4  lower 32 bits */
    uint32_t i_atime;               /* 8  */
    uint32_t i_ctime;               /* 12 */
    uint32_t i_mtime;               /* 16 */
    uint32_t i_dtime;               /* 20 */
    uint16_t i_gid;                 /* 24 */
    uint16_t i_links_count;         /* 26 */
    uint32_t i_blocks;              /* 28 in 512-byte sectors */
    uint32_t i_flags;               /* 32 */
    uint32_t i_osd1;                /* 36 */
    uint32_t i_block[15];           /* 40 block pointers (60 bytes) */
    uint32_t i_generation;          /* 100 */
    uint32_t i_file_acl;            /* 104 */
    uint32_t i_dir_acl;             /* 108 upper 32 bits of size (rev1 reg files) */
    uint32_t i_faddr;               /* 112 */
    uint8_t  i_osd2[12];            /* 116 */
};

/* Directory entry — variable length, packed sequentially in dir blocks. */
struct __attribute__((packed)) ext2_disk_dirent {
    uint32_t inode;                 /* 0  inode number (0 = deleted) */
    uint16_t rec_len;               /* 4  total record length */
    uint8_t  name_len;              /* 6  actual name length */
    uint8_t  file_type;             /* 7  EXT2_FT_* */
    /* name follows: name_len bytes, NOT null-terminated on disk */
};

/* i_mode type bits */
#define EXT2_S_IFSOCK   0xC000
#define EXT2_S_IFLNK    0xA000
#define EXT2_S_IFREG    0x8000
#define EXT2_S_IFBLK    0x6000
#define EXT2_S_IFDIR    0x4000
#define EXT2_S_IFCHR    0x2000
#define EXT2_S_IFIFO    0x1000
#define EXT2_S_IFMT     0xF000

/* Directory entry file_type values */
#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_CHRDEV   3
#define EXT2_FT_BLKDEV   4
#define EXT2_FT_FIFO     5
#define EXT2_FT_SOCK     6
#define EXT2_FT_SYMLINK  7

/* =====================================================================
 *  Runtime state
 * ===================================================================== */

/* Mount-private state — one per mounted ext2 partition. */
struct ext2_mount {
    struct ext2_block_dev *dev;
    uint64_t  part_lba;          /* LBA offset of partition start */
    uint32_t  block_size;        /* 1024 << s_log_block_size */
    uint32_t  inodes_per_group;
    uint32_t  blocks_per_group;
    uint32_t  inode_size;        /* 128 for rev0 */
    uint32_t  total_inodes;
    uint32_t  total_blocks;
    uint32_t  first_data_block;  /* 0 for bs>1K, 1 for bs=1K */
    uint8_t  *scratch;           /* block_size bytes, reusable read buffer */

    /* Indirect-block cache. Without this, inode_bmap() re-reads the
     * same indirect table from disk once per file block whenever a
     * caller walks a file sequentially (ext2_read_data()'s per-block
     * loop, directory iteration, path lookup) — the common case.
     * ind_l1 covers single-indirect (i_block[12]) and double-indirect's
     * top level (i_block[13]); ind_l2/ind_l3 cover the deeper levels of
     * double- and triple-indirect. Separate from `scratch` above:
     * `scratch` is clobbered by the actual data-block read in the same
     * ext2_read_data() iteration that calls inode_bmap(), so a cache
     * living there would be invalidated before the next call. A tag of
     * 0 means "nothing cached" — safe, since ext2 block 0 is always
     * the reserved boot block and never a valid indirect-table block. */
    uint8_t  *ind_l1;
    uint32_t  ind_l1_block;
    uint8_t  *ind_l2;
    uint32_t  ind_l2_block;
    uint8_t  *ind_l3;
    uint32_t  ind_l3_block;
};

/* Per-inode info — cached disk inode + metadata.  One per vfs_node in
 * kernel mode; used directly in host test mode. */
struct ext2_inode_info {
    struct ext2_mount      *mount;
    uint32_t                ino;     /* 1-based inode number */
    struct ext2_disk_inode  disk;    /* cached copy of on-disk inode */
};

/* Result from ext2_iter_dir(). */
struct ext2_dir_result {
    uint32_t ino;
    uint8_t  file_type;           /* EXT2_FT_* */
    char     name[256];           /* null-terminated */
};

/* =====================================================================
 *  Core API — independent of VFS types.
 *
 *  These functions are the same in both host-test and kernel builds.
 *  The kernel's VFS adapter (at the bottom of ext2.c, guarded by
 *  #ifndef EXT2_HOST_TEST) wraps them in vfs_node_ops / vfs_fs_ops
 *  callbacks.
 * ===================================================================== */

/* Read the superblock, validate magic, populate the mount struct.
 * Returns NULL on failure (bad magic, I/O error, allocation failure). */
struct ext2_mount *ext2_mount_create(struct ext2_block_dev *dev,
                                     uint64_t part_lba);
void ext2_mount_destroy(struct ext2_mount *m);

/* Look up an inode by number.  Allocates and returns an ext2_inode_info
 * with the disk inode read and cached.  Returns NULL on failure. */
struct ext2_inode_info *ext2_lookup_inode(struct ext2_mount *m, uint32_t ino);
void ext2_free_inode_info(struct ext2_inode_info *info);

/* Read file data at [off, off+count).  Returns bytes actually read
 * (may be less at EOF), or negative errno on error. */
long ext2_read_data(struct ext2_mount *m, struct ext2_inode_info *info,
                    size_t off, void *buf, size_t count);

/* Get the idx-th directory entry (0-based).  Returns 1 if an entry was
 * found and written to *out, 0 if idx is past the end of the directory,
 * or negative errno on I/O error.  Deleted entries (inode==0) are
 * silently skipped and do NOT consume an index. */
int ext2_iter_dir(struct ext2_mount *m, struct ext2_inode_info *info,
                  size_t idx, struct ext2_dir_result *out);

/* Read symlink target.  Returns the number of bytes written to buf
 * (not including a trailing NUL, which IS written if it fits), or
 * negative errno.  Handles both fast-path (target in inode block
 * pointers, when i_blocks==0 and i_size<=60) and slow-path (target
 * in a data block). */
long ext2_read_symlink(struct ext2_mount *m, struct ext2_inode_info *info,
                       void *buf, size_t bufsize);

/* Walk a '/'-separated path from the root directory (inode 2).
 * Returns a freshly allocated ext2_inode_info for the final component,
 * or NULL if any component is not found.  Symlinks in intermediate
 * components are NOT followed (this is a raw lookup, not a VFS-level
 * resolve — the kernel VFS handles symlink expansion itself). */
struct ext2_inode_info *ext2_lookup_path(struct ext2_mount *m,
                                         const char *path);

#endif /* EXT2_H */
