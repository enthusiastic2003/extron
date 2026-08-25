/*
 * ext2.c — Read-write ext2 filesystem driver.
 *
 * Designed to compile in two modes:
 *  1. Host test:  gcc -DEXT2_HOST_TEST  (uses malloc/free, pread-backed block dev)
 *  2. Kernel:     part of the kernel build  (uses kmalloc/kfree, EMMC block dev,
 *                 and exposes VFS ops tables at the bottom of this file)
 *
 * Limitations:
 *  - No Inode Cache / Dentry Cache: Every lookup operation reads the inode from disk 
 *    and creates a new in-memory `ext2_inode_info` and VFS `vfs_node`. If a file is 
 *    opened and subsequently modified or unlinked via a different path/lookup, the 
 *    open file descriptor's cached `vfs_node` will not see the changes (e.g. `fstat` 
 *    will report old link counts, and `readdir` on unlinked directories may read 
 *    freed blocks).
 *  - No Journaling: Modifications are synchronous and there is no ext3-style journal.
 *
 * The single #ifdef block below is the ONLY divergence.  All ext2 logic
 * beneath it is identical in both builds.
 */

/* ==================================================================== */
/*  Platform shim                                                       */
/* ==================================================================== */

#ifdef EXT2_HOST_TEST

#include "ext2.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#define kmalloc(sz)  malloc(sz)
#define kfree(p)     free(p)

/* Host printf for debug — disabled by default, flip to 1 to trace. */
#if 0
#define ext2_dbg(fmt, ...) fprintf(stderr, "[ext2] " fmt "\n", ##__VA_ARGS__)
#else
#define ext2_dbg(fmt, ...) ((void)0)
#endif

#else /* kernel build */

#include <kernel/fs/ext2.h>
#include <kernel/fs/vfs.h>
#include <kernel/mm/kheap.h>
#include <kernel/klibc/string.h>
#include <kernel/errno.h>
#include <kernel/console.h>

#define ext2_dbg(fmt, ...) ((void)0)

#endif /* EXT2_HOST_TEST */

/* ==================================================================== */
/*  On-disk layout assertions                                           */
/* ==================================================================== */

_Static_assert(sizeof(struct ext2_bgd) == 32,
               "ext2 block group descriptor must be 32 bytes");
_Static_assert(sizeof(struct ext2_disk_inode) == 128,
               "ext2 disk inode (base) must be 128 bytes");
_Static_assert(sizeof(struct ext2_disk_dirent) == 8,
               "ext2 dir entry header must be 8 bytes");

/* Superblock field offsets — if the packed struct drifts, the build fails. */
#include <stddef.h>
_Static_assert(offsetof(struct ext2_superblock, s_magic) == 56, "");
_Static_assert(offsetof(struct ext2_superblock, s_log_block_size) == 24, "");
_Static_assert(offsetof(struct ext2_superblock, s_inodes_per_group) == 40, "");
_Static_assert(offsetof(struct ext2_superblock, s_inode_size) == 88, "");
_Static_assert(offsetof(struct ext2_superblock, s_first_data_block) == 20, "");

/* Inode field offsets. */
_Static_assert(offsetof(struct ext2_disk_inode, i_mode) == 0, "");
_Static_assert(offsetof(struct ext2_disk_inode, i_size) == 4, "");
_Static_assert(offsetof(struct ext2_disk_inode, i_block) == 40, "");
_Static_assert(offsetof(struct ext2_disk_inode, i_blocks) == 28, "");

/* ==================================================================== */
/*  Low-level I/O                                                       */
/* ==================================================================== */

/*
 * Read `count` bytes from partition-relative `byte_offset` into `buf`.
 * Handles unaligned offsets and partial-sector reads by reading whole
 * sectors into a temporary buffer.  Allocates; suitable for infrequent
 * reads (superblock, inodes).
 */

static int ext2_sec_cache_get(struct ext2_mount *m, uint64_t lba, int *idx)
{
    int oldest = 0;
    uint32_t oldest_tick = 0xFFFFFFFF;
    
    for (int i = 0; i < 128; i++) {
        if (m->sec_cache[i].valid && m->sec_cache[i].lba == lba) {
            m->sec_cache[i].tick = ++m->sec_cache_tick;
            *idx = i;
            return 0;
        }
        if (!m->sec_cache[i].valid) {
            oldest = i;
            oldest_tick = 0;
        } else if (m->sec_cache[i].tick < oldest_tick) {
            oldest = i;
            oldest_tick = m->sec_cache[i].tick;
        }
    }
    
    if (m->sec_cache[oldest].valid && m->sec_cache[oldest].dirty) {
        m->dev->write_sectors(m->dev, m->sec_cache[oldest].lba, 1, m->sec_cache[oldest].data);
    }
    
    m->sec_cache[oldest].lba = lba;
    m->sec_cache[oldest].valid = true;
    m->sec_cache[oldest].dirty = false;
    m->sec_cache[oldest].tick = ++m->sec_cache_tick;
    
    int ret = m->dev->read_sectors(m->dev, lba, 1, m->sec_cache[oldest].data);
    if (ret < 0) {
        m->sec_cache[oldest].valid = false;
        return ret;
    }
    *idx = oldest;
    return 0;
}

static void ext2_sync_cache(struct ext2_mount *m)
{
    for (int i = 0; i < 128; i++) {
        if (m->sec_cache[i].valid && m->sec_cache[i].dirty) {
            m->dev->write_sectors(m->dev, m->sec_cache[i].lba, 1, m->sec_cache[i].data);
            m->sec_cache[i].dirty = false;
        }
    }
}

static int read_bytes(struct ext2_mount *m, uint64_t byte_offset,
                      void *buf, size_t count)
{
    uint32_t ss = m->dev->sector_size;
    if (ss != 512) return -ENOSYS;
    uint64_t sec_start = m->part_lba + (byte_offset / ss);
    size_t   sec_off   = (size_t)(byte_offset % ss);
    uint8_t *dst = (uint8_t *)buf;

    while (count > 0) {
        int idx;
        if (ext2_sec_cache_get(m, sec_start, &idx) < 0) return -EIO;
        size_t chunk = ss - sec_off;
        if (chunk > count) chunk = count;
        memcpy(dst, m->sec_cache[idx].data + sec_off, chunk);
        dst += chunk;
        count -= chunk;
        sec_start++;
        sec_off = 0;
    }
    return 0;
}

static int write_bytes(struct ext2_mount *m, uint64_t byte_offset,
                       const void *buf, size_t count)
{
    uint32_t ss = m->dev->sector_size;
    if (ss != 512) return -ENOSYS;
    uint64_t sec_start = m->part_lba + (byte_offset / ss);
    size_t   sec_off   = (size_t)(byte_offset % ss);
    const uint8_t *src = (const uint8_t *)buf;

    while (count > 0) {
        int idx;
        if (ext2_sec_cache_get(m, sec_start, &idx) < 0) return -EIO;
        size_t chunk = ss - sec_off;
        if (chunk > count) chunk = count;
        memcpy(m->sec_cache[idx].data + sec_off, src, chunk);
        m->sec_cache[idx].dirty = true;
        src += chunk;
        count -= chunk;
        sec_start++;
        sec_off = 0;
    }
    return 0;
}

static int read_block(struct ext2_mount *m, uint32_t block_no, void *buf)
{
    return read_bytes(m, (uint64_t)block_no * m->block_size, buf, m->block_size);
}

static int write_block(struct ext2_mount *m, uint32_t block_no, const void *buf)
{
    return write_bytes(m, (uint64_t)block_no * m->block_size, buf, m->block_size);
}
/* ==================================================================== */
/*  Inode lookup                                                        */
/* ==================================================================== */

/*
 * Read the block group descriptor for the group containing `ino`.
 */
static int read_bgd(struct ext2_mount *m, uint32_t group,
                    struct ext2_bgd *out)
{
    /* BGD table starts at the block immediately after the superblock
     * block.  For block_size == 1024: superblock is block 1, BGD table
     * is block 2.  For block_size > 1024: superblock is within block 0
     * at offset 1024, BGD table is block 1. In both cases the BGD table
     * block number is s_first_data_block + 1. */
    uint64_t bgd_byte = (uint64_t)(m->first_data_block + 1) * m->block_size
                       + (uint64_t)group * sizeof(struct ext2_bgd);
    return read_bytes(m, bgd_byte, out, sizeof(*out));
}

/*
 * Read the raw on-disk inode for inode number `ino` (1-based).
 */
static int read_disk_inode(struct ext2_mount *m, uint32_t ino,
                           struct ext2_disk_inode *out)
{
    if (ino == 0 || ino > m->total_inodes)
        return -EINVAL;

    uint32_t group = (ino - 1) / m->inodes_per_group;
    uint32_t index = (ino - 1) % m->inodes_per_group;

    struct ext2_bgd bgd;
    int ret = read_bgd(m, group, &bgd);
    if (ret < 0)
        return ret;

    uint64_t inode_byte = (uint64_t)bgd.bg_inode_table * m->block_size
                        + (uint64_t)index * m->inode_size;
    return read_bytes(m, inode_byte, out, sizeof(*out));
}


static int ext2_sync_bgd(struct ext2_mount *m, uint32_t group, const struct ext2_bgd *bgd)
{
    uint64_t bgd_byte = (uint64_t)(m->first_data_block + 1) * m->block_size
                       + (uint64_t)group * sizeof(struct ext2_bgd);
    return write_bytes(m, bgd_byte, bgd, sizeof(*bgd));
}

static int ext2_sync_inode(struct ext2_mount *m, struct ext2_inode_info *info)
{
    uint32_t group = (info->ino - 1) / m->inodes_per_group;
    uint32_t index = (info->ino - 1) % m->inodes_per_group;

    struct ext2_bgd bgd;
    int ret = read_bgd(m, group, &bgd);
    if (ret < 0) return ret;

    uint64_t inode_byte = (uint64_t)bgd.bg_inode_table * m->block_size
                        + (uint64_t)index * m->inode_size;
    return write_bytes(m, inode_byte, &info->disk, m->inode_size);
}

/* Find and allocate a free block. Updates bitmap and BGD, writes to disk. */
static int ext2_alloc_block(struct ext2_mount *m, uint32_t *out_block)
{
    uint32_t groups = (m->total_blocks + m->blocks_per_group - 1) / m->blocks_per_group;
    
    for (uint32_t g = 0; g < groups; g++) {
        struct ext2_bgd bgd;
        if (read_bgd(m, g, &bgd) < 0) continue;
        if (bgd.bg_free_blocks_count == 0) continue;

        /* Read the block bitmap */
        uint8_t *bitmap = (uint8_t *)kmalloc(m->block_size);
        if (!bitmap) return -ENOMEM;
        
        if (read_block(m, bgd.bg_block_bitmap, bitmap) < 0) {
            kfree(bitmap);
            continue;
        }

        /* Find first free bit */
        uint32_t limit = m->blocks_per_group;
        if (g == groups - 1) {
            limit = m->total_blocks % m->blocks_per_group;
            if (limit == 0) limit = m->blocks_per_group;
        }

        for (uint32_t bit = 0; bit < limit; bit++) {
            uint32_t byte_idx = bit / 8;
            uint32_t bit_idx = bit % 8;
            if (!(bitmap[byte_idx] & (1 << bit_idx))) {
                /* Free! Allocate it. */
                bitmap[byte_idx] |= (1 << bit_idx);
                write_block(m, bgd.bg_block_bitmap, bitmap);
                
                bgd.bg_free_blocks_count--;
                ext2_sync_bgd(m, g, &bgd);
                
                /* Update superblock */
                struct ext2_superblock sb;
                if (read_bytes(m, 1024, &sb, sizeof(sb)) == 0) {
                    sb.s_free_blocks_count--;
                    write_bytes(m, 1024, &sb, sizeof(sb));
                }

                kfree(bitmap);
                
                /* Calculate absolute block number */
                uint32_t block_no = g * m->blocks_per_group + m->first_data_block + bit;
                
                /* Zero out the newly allocated block for safety */
                uint8_t *zeros = (uint8_t *)kmalloc(m->block_size);
                if (zeros) {
                    memset(zeros, 0, m->block_size);
                    write_block(m, block_no, zeros);
                    kfree(zeros);
                }
                
                *out_block = block_no;
                return 0;
            }
        }
        kfree(bitmap);
    }
    return -ENOSPC;
}


/* ==================================================================== */
/*  Block mapping (file offset → disk block)                            */
/* ==================================================================== */

/*
 * Read block `block_no` into `cache_buf`, skipping the read entirely
 * if it's already there (tracked by `*cache_tag`). See the cache
 * fields' comment in ext2.h for why this exists and why 0 is a safe
 * "nothing cached" sentinel.
 */
static int read_block_cached(struct ext2_mount *m, uint32_t block_no,
                             uint8_t *cache_buf, uint32_t *cache_tag)
{
    if (*cache_tag == block_no)
        return 0;
    int ret = read_block(m, block_no, cache_buf);
    if (ret < 0) {
        *cache_tag = 0;  /* don't leave a stale tag pointing at bad data */
        return ret;
    }
    *cache_tag = block_no;
    return 0;
}

/*
 * Resolve a file-relative block number to an absolute disk block number.
 * Handles direct, single-indirect, double-indirect, and triple-indirect.
 *
 * Returns 0 on success (*out_block set; *out_block == 0 means a hole),
 * or negative errno on I/O error.
 *
 * Uses m->scratch as a temporary read buffer — caller must not assume
 * scratch contents are preserved across this call.
 */
static int inode_bmap(struct ext2_mount *m, struct ext2_disk_inode *di,
                      uint32_t file_block, uint32_t *out_block)
{
    uint32_t ptrs = m->block_size / 4;  /* pointers per indirect block */
    uint32_t n = file_block;

    /* Direct blocks: 0 .. 11 */
    if (n < 12) {
        *out_block = di->i_block[n];
        return 0;
    }
    n -= 12;

    /* Single indirect: 12 .. 12 + ptrs - 1 */
    if (n < ptrs) {
        if (di->i_block[12] == 0) { *out_block = 0; return 0; }
        int ret = read_block_cached(m, di->i_block[12], m->ind_l1, &m->ind_l1_block);
        if (ret < 0) return ret;
        *out_block = ((uint32_t *)m->ind_l1)[n];
        return 0;
    }
    n -= ptrs;

    /* Double indirect: up to ptrs * ptrs */
    if (n < ptrs * ptrs) {
        if (di->i_block[13] == 0) { *out_block = 0; return 0; }
        int ret = read_block_cached(m, di->i_block[13], m->ind_l1, &m->ind_l1_block);
        if (ret < 0) return ret;

        uint32_t ind_block = ((uint32_t *)m->ind_l1)[n / ptrs];
        if (ind_block == 0) { *out_block = 0; return 0; }

        ret = read_block_cached(m, ind_block, m->ind_l2, &m->ind_l2_block);
        if (ret < 0) return ret;
        *out_block = ((uint32_t *)m->ind_l2)[n % ptrs];
        return 0;
    }
    n -= ptrs * ptrs;

    /* Triple indirect: up to ptrs * ptrs * ptrs */
    if (n < ptrs * ptrs * ptrs) {
        if (di->i_block[14] == 0) { *out_block = 0; return 0; }
        int ret = read_block_cached(m, di->i_block[14], m->ind_l1, &m->ind_l1_block);
        if (ret < 0) return ret;

        uint32_t dind = ((uint32_t *)m->ind_l1)[n / (ptrs * ptrs)];
        if (dind == 0) { *out_block = 0; return 0; }

        ret = read_block_cached(m, dind, m->ind_l2, &m->ind_l2_block);
        if (ret < 0) return ret;

        uint32_t sind = ((uint32_t *)m->ind_l2)[(n / ptrs) % ptrs];
        if (sind == 0) { *out_block = 0; return 0; }

        ret = read_block_cached(m, sind, m->ind_l3, &m->ind_l3_block);
        if (ret < 0) return ret;

        *out_block = ((uint32_t *)m->ind_l3)[n % ptrs];
        return 0;
    }

    return -EINVAL;  /* file block beyond addressable range */
}

/* ==================================================================== */
/*  Public API: mount                                                   */
/* ==================================================================== */

struct ext2_mount *ext2_mount_create(struct ext2_block_dev *dev,
                                     uint64_t part_lba)
{
    if (!dev || !dev->read_sectors)
        return NULL;

    struct ext2_mount *m = (struct ext2_mount *)kmalloc(sizeof(*m));
    if (!m)
        return NULL;
    memset(m, 0, sizeof(*m));
    m->dev = dev;
    m->part_lba = part_lba;
    /* Temporarily set sector_size so read_bytes works before block_size
     * is known.  sector_size is only needed for the division in read_bytes. */

    /* Read superblock at byte 1024 within the partition. */
    uint8_t sb_buf[512];
    int ret = dev->read_sectors(dev, part_lba + 1024 / dev->sector_size, 1, sb_buf);
    struct ext2_superblock sb;
    memcpy(&sb, sb_buf, sizeof(sb));
    /* The superblock starts at byte 1024.  For sector_size 512, that's
     * sector 2.  But read_sectors fills from byte 0 of the sector range,
     * so if sector 2 is the first sector read, the superblock starts at
     * byte 0 of the read buffer.  This is only true if 1024 is sector-
     * aligned, which it always is for sector_size <= 1024. */
    if (ret < 0) {
        ext2_dbg("failed to read superblock sectors");
        kfree(m);
        return NULL;
    }

    if (sb.s_magic != EXT2_SUPER_MAGIC) {
        ext2_dbg("bad magic: 0x%04x (expected 0x%04x)",
                 sb.s_magic, EXT2_SUPER_MAGIC);
        kfree(m);
        return NULL;
    }

    m->block_size       = 1024u << sb.s_log_block_size;
    m->inodes_per_group = sb.s_inodes_per_group;
    m->blocks_per_group = sb.s_blocks_per_group;
    m->total_inodes     = sb.s_inodes_count;
    m->total_blocks     = sb.s_blocks_count;
    m->first_data_block = sb.s_first_data_block;

    /* rev0 always uses 128-byte inodes. */
    if (sb.s_rev_level >= 1 && sb.s_inode_size != 0)
        m->inode_size = sb.s_inode_size;
    else
        m->inode_size = 128;

    m->scratch = (uint8_t *)kmalloc(m->block_size);
    m->ind_l1  = (uint8_t *)kmalloc(m->block_size);
    m->ind_l2  = (uint8_t *)kmalloc(m->block_size);
    m->ind_l3  = (uint8_t *)kmalloc(m->block_size);
    if (!m->scratch || !m->ind_l1 || !m->ind_l2 || !m->ind_l3) {
        kfree(m->scratch);
        kfree(m->ind_l1);
        kfree(m->ind_l2);
        kfree(m->ind_l3);
        kfree(m);
        return NULL;
    }
    /* ind_l1_block/ind_l2_block/ind_l3_block start at 0 ("nothing
     * cached") via the memset(m, 0, ...) above. */

    ext2_dbg("mounted: block_size=%u inodes=%u blocks=%u ipg=%u",
             m->block_size, m->total_inodes, m->total_blocks,
             m->inodes_per_group);

    return m;
}

void ext2_mount_destroy(struct ext2_mount *m)
{
    if (!m) return;
    kfree(m->scratch);
    kfree(m->ind_l1);
    kfree(m->ind_l2);
    kfree(m->ind_l3);
    kfree(m);
}

/* ==================================================================== */
/*  Public API: inode operations                                        */
/* ==================================================================== */

struct ext2_inode_info *ext2_lookup_inode(struct ext2_mount *m, uint32_t ino)
{
    if (!m || ino == 0)
        return NULL;

    struct ext2_inode_info *info =
        (struct ext2_inode_info *)kmalloc(sizeof(*info));
    if (!info)
        return NULL;

    memset(info, 0, sizeof(*info));
    info->mount = m;
    info->ino   = ino;

    if (read_disk_inode(m, ino, &info->disk) < 0) {
        kfree(info);
        return NULL;
    }

    ext2_dbg("inode %u: mode=0%o size=%u links=%u blocks=%u",
             ino, info->disk.i_mode, info->disk.i_size,
             info->disk.i_links_count, info->disk.i_blocks);

    return info;
}

void ext2_free_inode_info(struct ext2_inode_info *info)
{
    kfree(info);
}

/* ==================================================================== */
/*  Public API: file data read                                          */
/* ==================================================================== */


/*
 * Like inode_bmap, but allocates blocks if they are missing.
 * Sets *dirty to true if the inode itself (i_block array) was modified.
 */
static int inode_bmap_alloc(struct ext2_mount *m, struct ext2_inode_info *info,
                            uint32_t file_block, uint32_t *out_block, bool *dirty)
{
    struct ext2_disk_inode *di = &info->disk;
    uint32_t ptrs = m->block_size / 4;
    uint32_t n = file_block;

    if (n < 12) {
        if (di->i_block[n] == 0) {
            uint32_t b;
            int ret = ext2_alloc_block(m, &b);
            di->i_block[n] = b;
            if (ret < 0) return ret;
            di->i_blocks += m->block_size / 512;
            *dirty = true;
        }
        *out_block = di->i_block[n];
        return 0;
    }
    n -= 12;

    if (n < ptrs) {
        if (di->i_block[12] == 0) {
            uint32_t b;
            int ret = ext2_alloc_block(m, &b);
            di->i_block[12] = b;
            if (ret < 0) return ret;
            di->i_blocks += m->block_size / 512;
            *dirty = true;
            m->ind_l1_block = di->i_block[12];
            memset(m->ind_l1, 0, m->block_size);
        }
        int ret = read_block_cached(m, di->i_block[12], m->ind_l1, &m->ind_l1_block);
        if (ret < 0) return ret;
        
        uint32_t *ind1 = (uint32_t *)m->ind_l1;
        if (ind1[n] == 0) {
            ret = ext2_alloc_block(m, &ind1[n]);
            if (ret < 0) return ret;
            di->i_blocks += m->block_size / 512;
            *dirty = true;
            write_block(m, di->i_block[12], m->ind_l1);
        }
        *out_block = ind1[n];
        return 0;
    }
    n -= ptrs;

    if (n < ptrs * ptrs) {
        if (di->i_block[13] == 0) {
            uint32_t b;
            int ret = ext2_alloc_block(m, &b);
            if (ret < 0) return ret;
            di->i_block[13] = b;
            di->i_blocks += m->block_size / 512;
            *dirty = true;
            m->ind_l1_block = di->i_block[13];
            memset(m->ind_l1, 0, m->block_size);
        }
        int ret = read_block_cached(m, di->i_block[13], m->ind_l1, &m->ind_l1_block);
        if (ret < 0) return ret;

        uint32_t *ind1 = (uint32_t *)m->ind_l1;
        uint32_t i = n / ptrs;
        uint32_t j = n % ptrs;

        if (ind1[i] == 0) {
            uint32_t b;
            ret = ext2_alloc_block(m, &b);
            if (ret < 0) return ret;
            ind1[i] = b;
            di->i_blocks += m->block_size / 512;
            *dirty = true;
            write_block(m, di->i_block[13], m->ind_l1);
            m->ind_l2_block = ind1[i];
            memset(m->ind_l2, 0, m->block_size);
        }
        ret = read_block_cached(m, ind1[i], m->ind_l2, &m->ind_l2_block);
        if (ret < 0) return ret;

        uint32_t *ind2 = (uint32_t *)m->ind_l2;
        if (ind2[j] == 0) {
            ret = ext2_alloc_block(m, &ind2[j]);
            if (ret < 0) return ret;
            di->i_blocks += m->block_size / 512;
            *dirty = true;
            write_block(m, ind1[i], m->ind_l2);
        }
        *out_block = ind2[j];
        return 0;
    }
    n -= ptrs * ptrs;

    if (n < ptrs * ptrs * ptrs) {
        if (di->i_block[14] == 0) {
            uint32_t b;
            int ret = ext2_alloc_block(m, &b);
            if (ret < 0) return ret;
            di->i_block[14] = b;
            di->i_blocks += m->block_size / 512;
            *dirty = true;
            m->ind_l1_block = di->i_block[14];
            memset(m->ind_l1, 0, m->block_size);
        }
        int ret = read_block_cached(m, di->i_block[14], m->ind_l1, &m->ind_l1_block);
        if (ret < 0) return ret;

        uint32_t *ind1 = (uint32_t *)m->ind_l1;
        uint32_t i = n / (ptrs * ptrs);
        uint32_t rem = n % (ptrs * ptrs);
        uint32_t j = rem / ptrs;
        uint32_t k = rem % ptrs;

        if (ind1[i] == 0) {
            uint32_t b;
            ret = ext2_alloc_block(m, &b);
            if (ret < 0) return ret;
            ind1[i] = b;
            di->i_blocks += m->block_size / 512;
            *dirty = true;
            write_block(m, di->i_block[14], m->ind_l1);
            m->ind_l2_block = ind1[i];
            memset(m->ind_l2, 0, m->block_size);
        }
        ret = read_block_cached(m, ind1[i], m->ind_l2, &m->ind_l2_block);
        if (ret < 0) return ret;

        uint32_t *ind2 = (uint32_t *)m->ind_l2;
        if (ind2[j] == 0) {
            uint32_t b;
            ret = ext2_alloc_block(m, &b);
            if (ret < 0) return ret;
            ind2[j] = b;
            di->i_blocks += m->block_size / 512;
            *dirty = true;
            write_block(m, ind1[i], m->ind_l2);
            m->ind_l3_block = ind2[j];
            memset(m->ind_l3, 0, m->block_size);
        }
        ret = read_block_cached(m, ind2[j], m->ind_l3, &m->ind_l3_block);
        if (ret < 0) return ret;

        uint32_t *ind3 = (uint32_t *)m->ind_l3;
        if (ind3[k] == 0) {
            ret = ext2_alloc_block(m, &ind3[k]);
            if (ret < 0) return ret;
            di->i_blocks += m->block_size / 512;
            *dirty = true;
            write_block(m, ind2[j], m->ind_l3);
        }
        *out_block = ind3[k];
        return 0;
    }
    
    return -ENOSYS; 
}

long ext2_write_data(struct ext2_mount *m, struct ext2_inode_info *info,
                     size_t off, const void *buf, size_t count)
{
    if (!m || !info || !buf) return -EINVAL;
    if ((info->disk.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return -EISDIR;
    if (count == 0) return 0;

    const uint8_t *src = (const uint8_t *)buf;
    size_t total_written = 0;
    bool inode_dirty = false;

    while (count > 0) {
        uint32_t file_block = off / m->block_size;
        size_t   block_off  = off % m->block_size;
        size_t   chunk      = m->block_size - block_off;
        if (chunk > count) chunk = count;

        uint32_t disk_block;
        int ret = inode_bmap_alloc(m, info, file_block, &disk_block, &inode_dirty);
        if (ret < 0) {
            if (total_written > 0) break;
            return ret;
        }

        if (chunk == m->block_size) {
            ret = write_block(m, disk_block, src);
        } else {
            ret = read_block(m, disk_block, m->scratch);
            if (ret == 0) {
                memcpy(m->scratch + block_off, src, chunk);
                ret = write_block(m, disk_block, m->scratch);
            }
        }

        if (ret < 0) {
            if (total_written > 0) break;
            return ret;
        }

        src           += chunk;
        off           += chunk;
        count         -= chunk;
        total_written += chunk;
    }

    if (off > info->disk.i_size) {
        info->disk.i_size = off;
        inode_dirty = true;
    }

    if (inode_dirty) {
        ext2_sync_inode(m, info);
    }

        ext2_sync_cache(m);
return (long)total_written;
}

long ext2_read_data(struct ext2_mount *m, struct ext2_inode_info *info,
                    size_t off, void *buf, size_t count)
{
    if (!m || !info || !buf)
        return -EINVAL;

    /* Directories should not be read as files. */
    if ((info->disk.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
        return -EISDIR;

    uint32_t file_size = info->disk.i_size;
    if (off >= file_size)
        return 0;
    if (off + count > file_size)
        count = file_size - off;
    if (count == 0)
        return 0;

    uint8_t *dst = (uint8_t *)buf;
    size_t total_read = 0;

    while (count > 0) {
        uint32_t file_block = (uint32_t)(off / m->block_size);
        size_t   block_off  = off % m->block_size;
        size_t   chunk      = m->block_size - block_off;
        if (chunk > count)
            chunk = count;

        uint32_t disk_block;
        int ret = inode_bmap(m, &info->disk, file_block, &disk_block);
        if (ret < 0)
            return ret;

        if (disk_block == 0) {
            /* Hole in a sparse file — read as zeros. */
            memset(dst, 0, chunk);
        } else {
            ret = read_block(m, disk_block, m->scratch);
            if (ret < 0)
                return ret;
            memcpy(dst, m->scratch + block_off, chunk);
        }

        dst        += chunk;
        off        += chunk;
        count      -= chunk;
        total_read += chunk;
    }

    return (long)total_read;
}

/* ==================================================================== */
/*  Public API: directory iteration                                     */
/* ==================================================================== */

int ext2_iter_dir(struct ext2_mount *m, struct ext2_inode_info *info,
                  size_t idx, struct ext2_dir_result *out)
{
    if (!m || !info || !out)
        return -EINVAL;

    if ((info->disk.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return -ENOTDIR;

    uint32_t dir_size = info->disk.i_size;
    uint32_t pos = 0;
    size_t   visible_idx = 0;

    while (pos < dir_size) {
        /* Which block are we in? */
        uint32_t file_block = pos / m->block_size;
        size_t   block_off  = pos % m->block_size;

        uint32_t disk_block;
        int ret = inode_bmap(m, &info->disk, file_block, &disk_block);
        if (ret < 0)
            return ret;
        if (disk_block == 0)
            break;  /* shouldn't happen for directories */

        ret = read_block(m, disk_block, m->scratch);
        if (ret < 0)
            return ret;

        /* Walk entries within this block starting from block_off. */
        while (block_off < m->block_size && pos < dir_size) {
            struct ext2_disk_dirent *de =
                (struct ext2_disk_dirent *)(m->scratch + block_off);

            if (de->rec_len == 0)
                return 0;  /* corrupt — bail */

            if (de->inode != 0) {
                /* Live entry. */
                if (visible_idx == idx) {
                    out->ino = de->inode;
                    out->file_type = de->file_type;
                    size_t nlen = de->name_len;
                    if (nlen > 255) nlen = 255;
                    memcpy(out->name, (uint8_t *)de + 8, nlen);
                    out->name[nlen] = '\0';
                    return 1;
                }
                visible_idx++;
            }

            pos       += de->rec_len;
            block_off += de->rec_len;
        }
    }

    return 0;  /* past the end */
}

/* ==================================================================== */
/*  Public API: symlink read                                            */
/* ==================================================================== */

long ext2_read_symlink(struct ext2_mount *m, struct ext2_inode_info *info,
                       void *buf, size_t bufsize)
{
    if (!m || !info || !buf)
        return -EINVAL;

    if ((info->disk.i_mode & EXT2_S_IFMT) != EXT2_S_IFLNK)
        return -EINVAL;

    uint32_t target_len = info->disk.i_size;

    /* Fast-path: target stored directly in i_block[] (up to 60 bytes).
     * The reliable indicator is i_blocks == 0: no data blocks were
     * allocated, so the data must be in the inode itself. */
    if (info->disk.i_blocks == 0 && target_len <= 60) {
        size_t copy = target_len;
        if (copy > bufsize)
            copy = bufsize;
        memcpy(buf, info->disk.i_block, copy);
        /* NUL-terminate if room. */
        if (copy < bufsize)
            ((uint8_t *)buf)[copy] = '\0';
        return (long)copy;
    }

    /* Slow path: target in a data block.  ext2_read_data only rejects
     * IFDIR, so IFLNK passes through correctly. */
    size_t copy = target_len;
    if (copy > bufsize)
        copy = bufsize;

    long ret = ext2_read_data(m, info, 0, buf, copy);
    if (ret < 0)
        return ret;

    if (ret < (long)bufsize)
        ((uint8_t *)buf)[ret] = '\0';
    return ret;
}

/* ==================================================================== */
/*  Public API: path lookup                                             */
/* ==================================================================== */

/*
 * Look up a child by name within a directory inode.
 * Returns a new ext2_inode_info for the child, or NULL.
 */
static struct ext2_inode_info *lookup_child(struct ext2_mount *m,
                                            struct ext2_inode_info *dir,
                                            const char *name, size_t namelen)
{
    if ((dir->disk.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return NULL;

    uint32_t dir_size = dir->disk.i_size;
    uint32_t pos = 0;

    while (pos < dir_size) {
        uint32_t file_block = pos / m->block_size;
        size_t   block_off  = pos % m->block_size;

        uint32_t disk_block;
        int ret = inode_bmap(m, &dir->disk, file_block, &disk_block);
        if (ret < 0 || disk_block == 0)
            return NULL;

        ret = read_block(m, disk_block, m->scratch);
        if (ret < 0)
            return NULL;

        while (block_off < m->block_size && pos < dir_size) {
            struct ext2_disk_dirent *de =
                (struct ext2_disk_dirent *)(m->scratch + block_off);

            if (de->rec_len == 0)
                return NULL;

            if (de->inode != 0
                && de->name_len == namelen
                && memcmp((uint8_t *)de + 8, name, namelen) == 0)
            {
                return ext2_lookup_inode(m, de->inode);
            }

            pos       += de->rec_len;
            block_off += de->rec_len;
        }
    }

    return NULL;
}

struct ext2_inode_info *ext2_lookup_path(struct ext2_mount *m,
                                         const char *path)
{
    if (!m || !path)
        return NULL;

    struct ext2_inode_info *cur = ext2_lookup_inode(m, EXT2_ROOT_INO);
    if (!cur)
        return NULL;

    /* Skip leading slash(es). */
    while (*path == '/')
        path++;

    /* Empty path after stripping slashes → root. */
    if (*path == '\0')
        return cur;

    while (*path) {
        /* Find the end of this component. */
        const char *comp = path;
        while (*path && *path != '/')
            path++;
        size_t len = (size_t)(path - comp);

        /* Skip trailing slashes. */
        while (*path == '/')
            path++;

        if (len == 0)
            continue;

        struct ext2_inode_info *child = lookup_child(m, cur, comp, len);
        ext2_free_inode_info(cur);
        if (!child)
            return NULL;

        cur = child;
    }

    return cur;
}

/* ==================================================================== */
/*  VFS integration layer — only compiled inside the kernel             */
/* ==================================================================== */

#ifndef EXT2_HOST_TEST

/* --- vfs_node_ops --- */




static long ext2_node_read(struct vfs_node *node, size_t off,
                           void *buf, size_t count)
{
    struct ext2_inode_info *info = (struct ext2_inode_info *)node->private;
    return ext2_read_data(info->mount, info, off, buf, count);
}

static int ext2_node_readdir(struct vfs_node *node, size_t index,
                             struct vfs_dirent *entry)
{
    struct ext2_inode_info *info = (struct ext2_inode_info *)node->private;
    struct ext2_dir_result res;
    int ret = ext2_iter_dir(info->mount, info, index, &res);
    if (ret <= 0)
        return ret;

    entry->ino = res.ino;
    /* Map ext2 file_type → vfs_node_type. */
    switch (res.file_type) {
        case EXT2_FT_DIR:      entry->type = VFS_NODE_DIRECTORY; break;
        case EXT2_FT_SYMLINK:  entry->type = VFS_NODE_SYMLINK;  break;
        case EXT2_FT_REG_FILE: entry->type = VFS_NODE_REGULAR;  break;
        default:               entry->type = VFS_NODE_REGULAR;   break;
    }
    memcpy(entry->name, res.name, strlen(res.name) + 1);
    return 1;
}

static int ext2_node_getattr(struct vfs_node *node, struct vfs_attr *attr)
{
    struct ext2_inode_info *info = (struct ext2_inode_info *)node->private;
    struct ext2_disk_inode *di = &info->disk;

    memset(attr, 0, sizeof(*attr));
    attr->ino   = info->ino;
    attr->type  = node->type;
    attr->mode  = di->i_mode & 07777;
    attr->nlink = di->i_links_count;
    attr->uid   = di->i_uid;
    attr->gid   = di->i_gid;
    attr->size  = di->i_size;
    attr->atime.sec = di->i_atime;
    attr->mtime.sec = di->i_mtime;
    attr->ctime.sec = di->i_ctime;
    return 0;
}




static long ext2_node_readlink(struct vfs_node *node, void *buf, size_t size)
{
    struct ext2_inode_info *info = (struct ext2_inode_info *)node->private;
    return ext2_read_symlink(info->mount, info, buf, size);
}

static void ext2_node_destroy(struct vfs_node *node)
{
    struct ext2_inode_info *info = (struct ext2_inode_info *)node->private;
    ext2_free_inode_info(info);
    kfree(node);
}



/* ==================================================================== */
/*  File Deletion (Phase 3)                                             */
/* ==================================================================== */

static void ext2_free_block(struct ext2_mount *m, uint32_t block)
{
    if (block < m->first_data_block || block >= m->total_blocks) return;
    uint32_t bg = (block - m->first_data_block) / m->blocks_per_group;
    uint32_t bit = (block - m->first_data_block) % m->blocks_per_group;
    
    struct ext2_bgd bgd;
    if (read_bgd(m, bg, &bgd) < 0) return;
    
    uint8_t *bitmap = kmalloc(m->block_size);
    if (!bitmap) return;
    
    if (read_block(m, bgd.bg_block_bitmap, bitmap) == 0) {
        bitmap[bit / 8] &= ~(1 << (bit % 8));
        write_block(m, bgd.bg_block_bitmap, bitmap);
        
        bgd.bg_free_blocks_count++;
        ext2_sync_bgd(m, bg, &bgd);
        
        struct ext2_superblock sb;
        if (read_bytes(m, 1024, &sb, sizeof(sb)) == 0) {
            sb.s_free_blocks_count++;
            write_bytes(m, 1024, &sb, sizeof(sb));
        }
    }
    kfree(bitmap);
}

static void ext2_free_inode(struct ext2_mount *m, uint32_t ino, bool is_dir)
{
    if (ino < 1 || ino > m->total_inodes) return;
    uint32_t bg = (ino - 1) / m->inodes_per_group;
    uint32_t bit = (ino - 1) % m->inodes_per_group;
    
    struct ext2_bgd bgd;
    if (read_bgd(m, bg, &bgd) < 0) return;
    
    uint8_t *bitmap = kmalloc(m->block_size);
    if (!bitmap) return;
    
    if (read_block(m, bgd.bg_inode_bitmap, bitmap) == 0) {
        bitmap[bit / 8] &= ~(1 << (bit % 8));
        write_block(m, bgd.bg_inode_bitmap, bitmap);
        
        bgd.bg_free_inodes_count++;
        if (is_dir && bgd.bg_used_dirs_count > 0) {
            bgd.bg_used_dirs_count--;
        }
        ext2_sync_bgd(m, bg, &bgd);
        
        struct ext2_superblock sb;
        if (read_bytes(m, 1024, &sb, sizeof(sb)) == 0) {
            sb.s_free_inodes_count++;
            write_bytes(m, 1024, &sb, sizeof(sb));
        }
        
        /* Wipe the inode struct */
        struct ext2_inode_info *info = ext2_lookup_inode(m, ino);
        if (info) {
            memset(&info->disk, 0, sizeof(info->disk));
            ext2_sync_inode(m, info);
            ext2_free_inode_info(info);
        }
    }
    kfree(bitmap);
}

static int ext2_remove_dirent(struct ext2_mount *m, struct ext2_inode_info *dir, const char *name)
{
    size_t namelen = strlen(name);
    uint32_t dir_size = dir->disk.i_size;
    uint32_t pos = 0;
    
    while (pos < dir_size) {
        uint32_t file_block = pos / m->block_size;
        size_t   block_off  = pos % m->block_size;
        
        uint32_t disk_block;
        if (inode_bmap(m, &dir->disk, file_block, &disk_block) < 0 || disk_block == 0) {
            return -EIO;
        }
        if (read_block(m, disk_block, m->scratch) < 0) return -EIO;
        
        struct ext2_disk_dirent *prev = NULL;
        while (block_off < m->block_size && pos < dir_size) {
            struct ext2_disk_dirent *de = (struct ext2_disk_dirent *)(m->scratch + block_off);
            if (de->rec_len == 0) return -EIO;
            
            if (de->inode != 0 && de->name_len == namelen && memcmp((uint8_t *)de + 8, name, namelen) == 0) {
                /* Found it! */
                if (prev) {
                    /* Absorb into previous entry */
                    prev->rec_len += de->rec_len;
                } else {
                    /* First entry in the block, just zero the inode */
                    de->inode = 0;
                }
                if (write_block(m, disk_block, m->scratch) < 0) return -EIO;
                ext2_sync_inode(m, dir);
                return 0;
            }
            
            prev = de;
            pos       += de->rec_len;
            block_off += de->rec_len;
        }
    }
    return -ENOENT;
}

static int ext2_node_truncate(struct vfs_node *node, size_t size)
{
    struct ext2_inode_info *info = (struct ext2_inode_info *)node->private;
    struct ext2_mount *m = info->mount;
    if (size != 0) return -ENOSYS;

    struct ext2_disk_inode *di = &info->disk;
    uint32_t ptrs = m->block_size / 4;

    /* Invalidate the indirect-block cache unconditionally before
     * freeing anything below: every block this function frees can be
     * handed straight back out by ext2_alloc_block()'s first-fit scan
     * (e.g. a subsequent write to this same file, or an unrelated one),
     * and a stale ind_l1/l2/l3 cache entry still tagged with that block
     * number would silently hand back the wrong content on the next
     * lookup instead of the new owner's data. Cheap to invalidate
     * broadly here — worst case is one extra real read to repopulate
     * it later. Only inode_bmap()'s cache is affected; m->scratch
     * holds no cross-call state of its own. */
    m->ind_l1_block = 0;
    m->ind_l2_block = 0;
    m->ind_l3_block = 0;

    /* Free direct blocks */
    for (int i = 0; i < 12; i++) {
        if (di->i_block[i]) {
            ext2_free_block(m, di->i_block[i]);
            di->i_block[i] = 0;
        }
    }

    /* Free single indirect */
    if (di->i_block[12]) {
        uint32_t *ind1 = kmalloc(m->block_size);
        if (ind1 && read_block(m, di->i_block[12], ind1) == 0) {
            for (uint32_t i = 0; i < ptrs; i++) {
                if (ind1[i]) ext2_free_block(m, ind1[i]);
            }
        }
        if (ind1) kfree(ind1);
        ext2_free_block(m, di->i_block[12]);
        di->i_block[12] = 0;
    }

    /* Free double indirect: read the dind table, and for each non-hole
     * leaf-table pointer it holds, free every data block that leaf
     * lists, then the leaf table itself, then finally the dind table.
     * Only reachable today via a file that had blocks placed in this
     * range from outside this kernel (inode_bmap_alloc() can't allocate
     * this far itself yet — see ext2_plan.md's "Current state"), but
     * unlink() must still be able to reclaim it. */
    if (di->i_block[13]) {
        uint32_t *dind = kmalloc(m->block_size);
        if (dind && read_block(m, di->i_block[13], dind) == 0) {
            for (uint32_t i = 0; i < ptrs; i++) {
                if (!dind[i]) continue;
                uint32_t *leaf = kmalloc(m->block_size);
                if (leaf && read_block(m, dind[i], leaf) == 0) {
                    for (uint32_t j = 0; j < ptrs; j++) {
                        if (leaf[j]) ext2_free_block(m, leaf[j]);
                    }
                }
                if (leaf) kfree(leaf);
                ext2_free_block(m, dind[i]);
            }
        }
        if (dind) kfree(dind);
        ext2_free_block(m, di->i_block[13]);
        di->i_block[13] = 0;
    }

    /* Free triple indirect: same idea, one level deeper — a tind table
     * of dind-table pointers, each of those a dind table of leaf-table
     * pointers, each leaf a table of data-block pointers. */
    if (di->i_block[14]) {
        uint32_t *tind = kmalloc(m->block_size);
        if (tind && read_block(m, di->i_block[14], tind) == 0) {
            for (uint32_t i = 0; i < ptrs; i++) {
                if (!tind[i]) continue;
                uint32_t *dind = kmalloc(m->block_size);
                if (dind && read_block(m, tind[i], dind) == 0) {
                    for (uint32_t j = 0; j < ptrs; j++) {
                        if (!dind[j]) continue;
                        uint32_t *leaf = kmalloc(m->block_size);
                        if (leaf && read_block(m, dind[j], leaf) == 0) {
                            for (uint32_t k = 0; k < ptrs; k++) {
                                if (leaf[k]) ext2_free_block(m, leaf[k]);
                            }
                        }
                        if (leaf) kfree(leaf);
                        ext2_free_block(m, dind[j]);
                    }
                }
                if (dind) kfree(dind);
                ext2_free_block(m, tind[i]);
            }
        }
        if (tind) kfree(tind);
        ext2_free_block(m, di->i_block[14]);
        di->i_block[14] = 0;
    }

    di->i_size = 0;
    di->i_blocks = 0;
    return ext2_sync_inode(m, info);
}

static long ext2_node_write(struct vfs_node *node, size_t off, const void *buf, size_t count)
{
    struct ext2_inode_info *info = (struct ext2_inode_info *)node->private;
    return ext2_write_data(info->mount, info, off, buf, count);
}


static int ext2_node_setattr(struct vfs_node *node, const struct vfs_setattr *attr) {
    struct ext2_inode_info *info = (struct ext2_inode_info *)node->private;
    bool dirty = false;

    if (attr->valid & VFS_SET_MODE) {
        info->disk.i_mode = (info->disk.i_mode & EXT2_S_IFMT) | (attr->mode & 07777);
        dirty = true;
    }
    if (attr->valid & VFS_SET_UID) {
        info->disk.i_uid = attr->uid;
        dirty = true;
    }
    if (attr->valid & VFS_SET_GID) {
        info->disk.i_gid = attr->gid;
        dirty = true;
    }
    if (attr->valid & VFS_SET_ATIME) {
        info->disk.i_atime = attr->atime.sec;
        dirty = true;
    }
    if (attr->valid & VFS_SET_MTIME) {
        info->disk.i_mtime = attr->mtime.sec;
        dirty = true;
    }

    if (dirty) {
        int ret = ext2_sync_inode(info->mount, info);
        ext2_sync_cache(info->mount);
        return ret;
    }
    ext2_sync_cache(info->mount);
    return 0;
}
static const struct vfs_node_ops ext2_node_ops = {

    .read     = ext2_node_read,
    .write    = ext2_node_write,
    .truncate = ext2_node_truncate,
    .readdir  = ext2_node_readdir,
    .getattr  = ext2_node_getattr,
    .readlink = ext2_node_readlink,
    .destroy  = ext2_node_destroy,
    .setattr = ext2_node_setattr,
};

/* --- Helpers for VFS fs_ops --- */

static enum vfs_node_type ext2_mode_to_vfs_type(uint16_t mode)
{
    switch (mode & EXT2_S_IFMT) {
        case EXT2_S_IFDIR: return VFS_NODE_DIRECTORY;
        case EXT2_S_IFLNK: return VFS_NODE_SYMLINK;
        default:            return VFS_NODE_REGULAR;
    }
}

static struct vfs_node *make_vfs_node(struct ext2_inode_info *info)
{
    struct vfs_node *node = (struct vfs_node *)kmalloc(sizeof(*node));
    if (!node)
        return NULL;
    enum vfs_node_type type = ext2_mode_to_vfs_type(info->disk.i_mode);
    vfs_node_init(node, &ext2_node_ops, info, type);
    return node;
}

/* --- vfs_fs_ops --- */

static int ext2_root(struct vfs_mount *mount, struct vfs_dentry **out)
{
    struct ext2_mount *m = (struct ext2_mount *)mount->private;
    struct ext2_inode_info *info = ext2_lookup_inode(m, EXT2_ROOT_INO);
    if (!info)
        return -EIO;

    struct vfs_node *node = make_vfs_node(info);
    if (!node) {
            ext2_sync_cache(m);
ext2_free_inode_info(info);
        return -ENOMEM;
    }

    struct vfs_dentry *dentry = (struct vfs_dentry *)kmalloc(sizeof(*dentry));
    if (!dentry) {
        kfree(node);
        ext2_free_inode_info(info);
        return -ENOMEM;
    }

    int ret = vfs_dentry_init(dentry, node, NULL, "", NULL);
    if (ret < 0) {
        kfree(dentry);
        kfree(node);
        ext2_free_inode_info(info);
        return ret;
    }
    *out = dentry;
    return 0;
}

static int ext2_lookup_child_vfs(struct vfs_mount *mount,
                                 struct vfs_dentry *parent,
                                 const char *name,
                                 struct vfs_dentry **out)
{
    struct ext2_mount *m = (struct ext2_mount *)mount->private;
    struct ext2_inode_info *pinfo =
        (struct ext2_inode_info *)parent->node->private;

    struct ext2_inode_info *cinfo =
        lookup_child(m, pinfo, name, strlen(name));
    if (!cinfo)
        return -ENOENT;

    struct vfs_node *node = make_vfs_node(cinfo);
    if (!node) {
        ext2_free_inode_info(cinfo);
        return -ENOMEM;
    }

    struct vfs_dentry *dentry = (struct vfs_dentry *)kmalloc(sizeof(*dentry));
    if (!dentry) {
        kfree(node);
        ext2_free_inode_info(cinfo);
        return -ENOMEM;
    }

    int ret = vfs_dentry_init(dentry, node, parent, name, NULL);
    if (ret < 0) {
        kfree(dentry);
        kfree(node);
        ext2_free_inode_info(cinfo);
        return ret;
    }
    *out = dentry;
    return 0;
}





static void ext2_destroy_dentry(struct vfs_dentry *dentry)
{
    kfree(dentry);
}


/* ==================================================================== */
/*  File Creation (Phase 3)                                             */
/* ==================================================================== */

static int ext2_alloc_inode(struct ext2_mount *m, uint16_t mode, uint32_t uid, uint32_t gid, uint32_t *out_ino)
{
    uint32_t groups = (m->total_blocks + m->blocks_per_group - 1) / m->blocks_per_group;
    
    for (uint32_t g = 0; g < groups; g++) {
        struct ext2_bgd bgd;
        if (read_bgd(m, g, &bgd) < 0) continue;
        if (bgd.bg_free_inodes_count == 0) continue;

        uint8_t *bitmap = (uint8_t *)kmalloc(m->block_size);
        if (!bitmap) return -ENOMEM;
        
        if (read_block(m, bgd.bg_inode_bitmap, bitmap) < 0) {
            kfree(bitmap);
            continue;
        }

        uint32_t limit = m->inodes_per_group;
        for (uint32_t bit = 0; bit < limit; bit++) {
            uint32_t byte_idx = bit / 8;
            uint32_t bit_idx = bit % 8;
            if (!(bitmap[byte_idx] & (1 << bit_idx))) {
                /* Free! Allocate it. */
                bitmap[byte_idx] |= (1 << bit_idx);
                write_block(m, bgd.bg_inode_bitmap, bitmap);
                
                bgd.bg_free_inodes_count--;
                if ((mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
                    bgd.bg_used_dirs_count++;
                }
                ext2_sync_bgd(m, g, &bgd);
                
                /* Update superblock in memory */
                struct ext2_superblock sb;
                if (read_bytes(m, 1024, &sb, sizeof(sb)) == 0) {
                    sb.s_free_inodes_count--;
                    write_bytes(m, 1024, &sb, sizeof(sb));
                }

                kfree(bitmap);
                
                uint32_t ino = g * m->inodes_per_group + bit + 1;
                
                /* Create a fresh ext2_inode_info and sync it to disk to clear old garbage */
                struct ext2_inode_info *info = kmalloc(sizeof(*info));
                if (!info) return -ENOMEM;
                memset(info, 0, sizeof(*info));
                info->mount = m;
                info->ino = ino;
                
                info->disk.i_mode = mode;
                info->disk.i_uid = uid;
                info->disk.i_gid = gid;
                info->disk.i_links_count = 1;
                // We'd set timestamps here if we had a RTC.
                
                ext2_sync_inode(m, info);
                kfree(info);
                
                *out_ino = ino;
                return 0;
            }
        }
        kfree(bitmap);
    }
    return -ENOSPC;
}

#define EXT2_DIR_REC_LEN(name_len) (((name_len) + 8 + 3) & ~3)

static int ext2_add_dirent(struct ext2_mount *m, struct ext2_inode_info *dir,
                           const char *name, uint32_t ino, uint8_t file_type)
{
    size_t namelen = strlen(name);
    if (namelen > 255) return -ENAMETOOLONG;
    
    uint16_t req_len = EXT2_DIR_REC_LEN(namelen);
    uint32_t dir_size = dir->disk.i_size;
    uint32_t pos = 0;
    
    /* Search existing blocks for slack space */
    while (pos < dir_size) {
        uint32_t file_block = pos / m->block_size;
        size_t   block_off  = pos % m->block_size;
        
        uint32_t disk_block;
        if (inode_bmap(m, &dir->disk, file_block, &disk_block) < 0 || disk_block == 0) {
            return -EIO;
        }
        
        if (read_block(m, disk_block, m->scratch) < 0) return -EIO;
        
        while (block_off < m->block_size && pos < dir_size) {
            struct ext2_disk_dirent *de = (struct ext2_disk_dirent *)(m->scratch + block_off);
            if (de->rec_len == 0) return -EIO; /* corrupt */
            
            /* Calculate the minimum space this existing entry needs */
            uint16_t min_len = (de->inode == 0) ? 0 : EXT2_DIR_REC_LEN(de->name_len);
            uint16_t slack = de->rec_len - min_len;
            
            if (slack >= req_len) {
                /* We can insert here! */
                if (min_len > 0) {
                    /* Shrink the existing entry */
                    de->rec_len = min_len;
                    /* Move pointer to the slack space */
                    de = (struct ext2_disk_dirent *)((uint8_t *)de + min_len);
                    de->rec_len = slack;
                }
                
                de->inode = ino;
                de->file_type = file_type;
                de->name_len = namelen;
                memcpy((uint8_t *)de + 8, name, namelen);
                
                if (write_block(m, disk_block, m->scratch) < 0) return -EIO;
                
                /* Update directory mtime / sync */
                ext2_sync_inode(m, dir);
                return 0;
            }
            
            pos       += de->rec_len;
            block_off += de->rec_len;
        }
    }
    
    /* If we get here, no existing block had enough slack space. We must allocate a new block. */
    uint32_t new_file_block = dir_size / m->block_size;
    uint32_t new_disk_block;
    bool dirty = false;
    
    if (inode_bmap_alloc(m, dir, new_file_block, &new_disk_block, &dirty) < 0) {
        return -ENOSPC;
    }
    
    memset(m->scratch, 0, m->block_size);
    struct ext2_disk_dirent *de = (struct ext2_disk_dirent *)m->scratch;
    de->inode = ino;
    de->rec_len = m->block_size;
    de->name_len = namelen;
    de->file_type = file_type;
    memcpy((uint8_t *)de + 8, name, namelen);
    
    if (write_block(m, new_disk_block, m->scratch) < 0) return -EIO;
    
    dir->disk.i_size += m->block_size;
    ext2_sync_inode(m, dir);
    
    return 0;
}


static int ext2_node_create(struct vfs_mount *vfs_m, struct vfs_dentry *parent_dentry, const char *name,
                            enum vfs_node_type type, uint32_t mode, uint32_t uid, uint32_t gid,
                            struct vfs_dentry **out)
{
    struct ext2_mount *m = (struct ext2_mount *)vfs_m->private;
    struct vfs_node *parent = parent_dentry->node;
    struct ext2_inode_info *dir = (struct ext2_inode_info *)parent->private;
    
    if (type != VFS_NODE_REGULAR && type != VFS_NODE_DIRECTORY) return -ENOSYS; /* Only files and dirs for now */

    struct vfs_dentry *existing = NULL;
    if (ext2_lookup_child_vfs(vfs_m, parent_dentry, name, &existing) == 0) {
        vfs_dentry_release(existing);
        return -EEXIST;
    }
    uint16_t ext2_mode = (type == VFS_NODE_DIRECTORY) ? (EXT2_S_IFDIR | mode) : (EXT2_S_IFREG | mode);
    uint8_t ext2_type = (type == VFS_NODE_DIRECTORY) ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
    
    uint32_t ino;
    int ret = ext2_alloc_inode(m, ext2_mode, uid, gid, &ino);
    if (ret < 0) return ret;
    
    ret = ext2_add_dirent(m, dir, name, ino, ext2_type);
    if (ret < 0) {
        /* Otherwise this inode stays marked used in the bitmap forever
         * with no directory entry pointing at it — a permanent leak,
         * since this project has no fsck to reclaim it later. */
        ext2_free_inode(m, ino, type == VFS_NODE_DIRECTORY);
        return ret;
    }
    
    if (type == VFS_NODE_DIRECTORY) {
        /* Directories must immediately be formatted with . and .. */
        uint32_t dir_block;
        if (ext2_alloc_block(m, &dir_block) == 0) {
            memset(m->scratch, 0, m->block_size);
            

            struct ext2_disk_dirent *dot = (struct ext2_disk_dirent *)m->scratch;
            dot->inode = ino;
            dot->rec_len = 12;
            dot->name_len = 1;
            dot->file_type = EXT2_FT_DIR;
            ((uint8_t *)dot + 8)[0] = '.';
            
            struct ext2_disk_dirent *dotdot = (struct ext2_disk_dirent *)(m->scratch + 12);
            dotdot->inode = dir->ino;
            dotdot->rec_len = m->block_size - 12;
            dotdot->name_len = 2;
            dotdot->file_type = EXT2_FT_DIR;
            ((uint8_t *)dotdot + 8)[0] = '.';
            ((uint8_t *)dotdot + 8)[1] = '.';
            
            write_block(m, dir_block, m->scratch);

            
            /* Update the new directory's inode */
            struct ext2_inode_info *new_dir = ext2_lookup_inode(m, ino);
            if (new_dir) {
                new_dir->disk.i_size = m->block_size;
                new_dir->disk.i_blocks = m->block_size / 512;
                new_dir->disk.i_block[0] = dir_block;
                new_dir->disk.i_links_count = 2; /* Itself and parent */
                ext2_sync_inode(m, new_dir);
                ext2_free_inode_info(new_dir);
            }
            
            /* Update parent's link count */
            dir->disk.i_links_count++;
            ext2_sync_inode(m, dir);
        }
    }

    
    /* Build the new VFS node */
    struct ext2_inode_info *child_info = ext2_lookup_inode(m, ino);
    if (!child_info) return -EIO;
    
    struct vfs_node *node = kmalloc(sizeof(*node));
    if (!node) {
        ext2_free_inode_info(child_info);
        return -ENOMEM;
    }
    memset(node, 0, sizeof(*node));
    node->type = type;
    node->ops = &ext2_node_ops;
    node->private = child_info;
    
    struct vfs_dentry *d = kmalloc(sizeof(*d));
    if (!d) {
        kfree(node);
        ext2_free_inode_info(child_info);
        return -ENOMEM;
    }
    ret = vfs_dentry_init(d, node, parent_dentry, name, NULL);
    if (ret < 0) {
        kfree(d);
        kfree(node);
        ext2_free_inode_info(child_info);
        return ret;
    }
    *out = d;
    ext2_sync_cache(m);
    return 0;
}


static int ext2_node_remove(struct vfs_mount *vfs_m, struct vfs_dentry *parent_dentry,
                            const char *name, int is_dir)
{
    struct ext2_mount *m = (struct ext2_mount *)vfs_m->private;
    struct vfs_node *parent = parent_dentry->node;
    struct ext2_inode_info *dir = (struct ext2_inode_info *)parent->private;

    struct ext2_inode_info *target = lookup_child(m, dir, name, strlen(name));
    if (!target) return -ENOENT;

    bool target_is_dir = ((target->disk.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR);
    if (is_dir && !target_is_dir) {
        ext2_free_inode_info(target);
        return -ENOTDIR;
    }
    if (!is_dir && target_is_dir) {
        ext2_free_inode_info(target);
        return -EISDIR;
    }

    if (is_dir) {
        /* Check if empty (size should be > 0 but we check if it has entries other than . and ..)
         * A fresh dir is 1 block, with . and .. taking the whole block.
         * If someone added files, the second entry (..) will have a smaller rec_len and followed by more. */
        bool empty = true;
        if (target->disk.i_size > 0 && target->disk.i_block[0]) {
            if (read_block(m, target->disk.i_block[0], m->scratch) == 0) {
                struct ext2_disk_dirent *de = (struct ext2_disk_dirent *)m->scratch;
                if (de->rec_len > 0 && de->rec_len < m->block_size) {
                    struct ext2_disk_dirent *de2 = (struct ext2_disk_dirent *)(m->scratch + de->rec_len);
                    if (de->rec_len + de2->rec_len < m->block_size) {
                        empty = false;
                    }
                }
            }
        }
        if (!empty) {
            ext2_free_inode_info(target);
            return -ENOTEMPTY;
        }
    }

    /* Decrement link count. Directories can never have more than one
     * hard link — ext2 disallows link() on a directory entirely (see
     * ext2_rofs_link()'s unconditional -EROFS) — so once confirmed
     * empty above, removing one always frees it outright; only
     * regular files need the general decrement-then-check-for-zero
     * dance, since they really can have multiple names. A fresh empty
     * directory's baseline count is 2 (itself's '.' plus the parent's
     * forward entry — see ext2_node_create()'s "new_dir->disk.i_links_
     * count = 2"), so treating it like a file and decrementing by only
     * 1 landed on 1, never reaching the free-on-zero branch below: the
     * directory's own inode and data block (its '.'/'..' block) never
     * got freed even after its parent's dirent was removed a few lines
     * down, leaving a live, fully-formed, but unreachable directory
     * behind — e2fsck calls this "unconnected": its own '..' still
     * correctly names the parent, but the parent no longer names it
     * back, and the leaked inode/block are never reused. */
    if (target_is_dir) {
        target->disk.i_links_count = 0;
    } else if (target->disk.i_links_count > 0) {
        target->disk.i_links_count--;
    }

    if (target->disk.i_links_count == 0) {
        /* Free all blocks (truncate to 0) */
        struct vfs_node dummy;
        dummy.private = target;
        ext2_node_truncate(&dummy, 0);
        /* Free the inode */
        ext2_free_inode(m, target->ino, is_dir);
    } else {
        ext2_sync_inode(m, target);
    }
    ext2_free_inode_info(target);

    /* Remove from parent directory */
    int ret = ext2_remove_dirent(m, dir, name);
    if (ret == 0 && is_dir && dir->disk.i_links_count > 0) {
        dir->disk.i_links_count--;
        ext2_sync_inode(m, dir);
    }

    ext2_sync_cache(m);
    return ret;
}



static int ext2_node_link(struct vfs_mount *vfs_m, struct vfs_node *target_node,
                          struct vfs_dentry *parent_dentry, const char *name,
                          struct vfs_dentry **out)
{
    struct ext2_mount *m = (struct ext2_mount *)vfs_m->private;
    struct ext2_inode_info *target = (struct ext2_inode_info *)target_node->private;
    struct ext2_inode_info *dir = (struct ext2_inode_info *)parent_dentry->node->private;
    
    uint16_t mode = target->disk.i_mode & EXT2_S_IFMT;
    if (mode == EXT2_S_IFDIR)
        return -EPERM;
        
    struct vfs_dentry *existing = NULL;
    if (ext2_lookup_child_vfs(vfs_m, parent_dentry, name, &existing) == 0) {
        vfs_dentry_release(existing);
        return -EEXIST;
    }

    uint8_t ext2_type = EXT2_FT_UNKNOWN;
    if (mode == EXT2_S_IFREG) ext2_type = EXT2_FT_REG_FILE;
    else if (mode == EXT2_S_IFCHR) ext2_type = EXT2_FT_CHRDEV;
    else if (mode == EXT2_S_IFBLK) ext2_type = EXT2_FT_BLKDEV;
    else if (mode == EXT2_S_IFIFO) ext2_type = EXT2_FT_FIFO;
    else if (mode == EXT2_S_IFSOCK) ext2_type = EXT2_FT_SOCK;
    else if (mode == EXT2_S_IFLNK) ext2_type = EXT2_FT_SYMLINK;
    
    int ret = ext2_add_dirent(m, dir, name, target->ino, ext2_type);
    if (ret < 0) return ret;
    
    target->disk.i_links_count++;
    ext2_sync_inode(m, target);
    ext2_sync_cache(m);
    
    if (out) {
        struct vfs_dentry *d = kmalloc(sizeof(*d));
        vfs_dentry_init(d, target_node, parent_dentry, name, NULL);
        *out = d;
    }
    
    return 0;
}

static int ext2_node_symlink(struct vfs_mount *vfs_m, struct vfs_dentry *parent_dentry,
                             const char *name, const char *target,
                             uint32_t uid, uint32_t gid,
                             struct vfs_dentry **out)
{
    struct ext2_mount *m = (struct ext2_mount *)vfs_m->private;
    struct ext2_inode_info *dir = (struct ext2_inode_info *)parent_dentry->node->private;
    
    struct vfs_dentry *existing = NULL;
    if (ext2_lookup_child_vfs(vfs_m, parent_dentry, name, &existing) == 0) {
        vfs_dentry_release(existing);
        return -EEXIST;
    }

    uint32_t ino;
    int ret = ext2_alloc_inode(m, EXT2_S_IFLNK | 0777, uid, gid, &ino);
    if (ret < 0) return ret;
    
    ret = ext2_add_dirent(m, dir, name, ino, EXT2_FT_SYMLINK);
    if (ret < 0) {
        ext2_free_inode(m, ino, 0);
        return ret;
    }
    
    struct ext2_inode_info *new_lnk = ext2_lookup_inode(m, ino);
    if (!new_lnk) return -EIO;
    
    size_t target_len = strlen(target);
    new_lnk->disk.i_size = target_len;
    
    if (target_len <= 60) {
        memcpy(new_lnk->disk.i_block, target, target_len);
        new_lnk->disk.i_blocks = 0;
    } else {
        uint32_t bnum;
        if (ext2_alloc_block(m, &bnum) == 0) {
            memset(m->scratch, 0, m->block_size);
            memcpy(m->scratch, target, target_len);
            write_block(m, bnum, m->scratch);
            new_lnk->disk.i_block[0] = bnum;
            new_lnk->disk.i_blocks = m->block_size / 512;
        } else {
            /* Handled badly: just make it an empty link */
            new_lnk->disk.i_size = 0;
        }
    }
    ext2_sync_inode(m, new_lnk);
    
    if (out) {
        struct vfs_node *node = make_vfs_node(new_lnk);
        if (!node) {
            ext2_free_inode_info(new_lnk);
            return -ENOMEM;
        }
        struct vfs_dentry *d = kmalloc(sizeof(*d));
        vfs_dentry_init(d, node, parent_dentry, name, NULL);
        *out = d;
    } else {
        ext2_free_inode_info(new_lnk);
    }
    
    ext2_sync_cache(m);
    return 0;
}

static int ext2_node_rename(struct vfs_mount *vfs_m, struct vfs_dentry *old_parent_d,
                            const char *old_name, struct vfs_dentry *new_parent_d,
                            const char *new_name)
{
    struct ext2_mount *m = (struct ext2_mount *)vfs_m->private;
    struct ext2_inode_info *old_dir = (struct ext2_inode_info *)old_parent_d->node->private;
    struct ext2_inode_info *new_dir = (struct ext2_inode_info *)new_parent_d->node->private;

    struct ext2_inode_info *target = lookup_child(m, old_dir, old_name, strlen(old_name));
    if (!target) return -ENOENT;

    bool target_is_dir = ((target->disk.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR);

    struct ext2_inode_info *existing = lookup_child(m, new_dir, new_name, strlen(new_name));
    if (existing) {
        bool existing_is_dir = ((existing->disk.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR);
        if (target_is_dir && !existing_is_dir) {
            ext2_free_inode_info(target); ext2_free_inode_info(existing); return -ENOTDIR;
        }
        if (!target_is_dir && existing_is_dir) {
            ext2_free_inode_info(target); ext2_free_inode_info(existing); return -EISDIR;
        }
        
        if (existing_is_dir) {
            bool empty = true;
            if (existing->disk.i_size > 0 && existing->disk.i_block[0]) {
                if (read_block(m, existing->disk.i_block[0], m->scratch) == 0) {
                    struct ext2_disk_dirent *de = (struct ext2_disk_dirent *)m->scratch;
                    if (de->rec_len > 0 && de->rec_len < m->block_size) {
                        struct ext2_disk_dirent *de2 = (struct ext2_disk_dirent *)(m->scratch + de->rec_len);
                        if (de->rec_len + de2->rec_len < m->block_size) empty = false;
                    }
                }
            }
            if (!empty) {
                ext2_free_inode_info(target); ext2_free_inode_info(existing); return -ENOTEMPTY;
            }
            ext2_remove_dirent(m, new_dir, new_name);
            existing->disk.i_links_count = 0;
            struct vfs_node dummy; dummy.private = existing; ext2_node_truncate(&dummy, 0);
            ext2_free_inode(m, existing->ino, 1);
            if (new_dir->disk.i_links_count > 0) new_dir->disk.i_links_count--;
        } else {
            ext2_remove_dirent(m, new_dir, new_name);
            if (existing->disk.i_links_count > 0) {
                existing->disk.i_links_count--;
                if (existing->disk.i_links_count == 0) {
                    struct vfs_node dummy; dummy.private = existing; ext2_node_truncate(&dummy, 0);
                    ext2_free_inode(m, existing->ino, 0);
                } else {
                    ext2_sync_inode(m, existing);
                }
            }
        }
        ext2_free_inode_info(existing);
    }

    uint8_t ext2_type = EXT2_FT_UNKNOWN;
    if (target_is_dir) ext2_type = EXT2_FT_DIR;
    else if ((target->disk.i_mode & EXT2_S_IFMT) == EXT2_S_IFREG) ext2_type = EXT2_FT_REG_FILE;
    else if ((target->disk.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK) ext2_type = EXT2_FT_SYMLINK;

    int ret = ext2_add_dirent(m, new_dir, new_name, target->ino, ext2_type);
    if (ret < 0) { ext2_free_inode_info(target); return ret; }

    ext2_remove_dirent(m, old_dir, old_name);

    if (target_is_dir && old_dir->ino != new_dir->ino) {
        if (target->disk.i_size > 0 && target->disk.i_block[0]) {
            if (read_block(m, target->disk.i_block[0], m->scratch) == 0) {
                struct ext2_disk_dirent *de = (struct ext2_disk_dirent *)m->scratch;
                if (de->rec_len > 0 && de->rec_len < m->block_size) {
                    struct ext2_disk_dirent *de2 = (struct ext2_disk_dirent *)(m->scratch + de->rec_len);
                    if (de2->name_len == 2 && de2->inode != 0) {
                        de2->inode = new_dir->ino;
                        write_block(m, target->disk.i_block[0], m->scratch);
                    }
                }
            }
        }
        if (old_dir->disk.i_links_count > 0) old_dir->disk.i_links_count--;
        new_dir->disk.i_links_count++;
        ext2_sync_inode(m, old_dir);
        ext2_sync_inode(m, new_dir);
    }

    ext2_free_inode_info(target);
    ext2_sync_cache(m);
    return 0;
}

const struct vfs_fs_ops ext2_fs_ops = {
    .root           = ext2_root,
    .lookup_child   = ext2_lookup_child_vfs,
    .create         = ext2_node_create,
    .remove         = ext2_node_remove,
    .rename         = ext2_node_rename,
    .link           = ext2_node_link,
    .symlink        = ext2_node_symlink,
    .destroy_dentry = ext2_destroy_dentry,
};

#endif /* !EXT2_HOST_TEST */
