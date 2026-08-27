#ifndef KERNEL_FS_VFS_H
#define KERNEL_FS_VFS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <kernel/sync/spinlock.h>

#define VFS_NAME_MAX 255
#define VFS_PATH_MAX 4096
#define VFS_MAX_MOUNTS 8
#define VFS_SYMLINK_MAX 40
#define VFS_GROUP_MAX 16

/* Filesystem-wide information, deliberately independent of any userspace
 * statvfs ABI. Filesystem drivers fill this and the syscall layer translates
 * it at the user/kernel boundary. */
struct vfs_fs_info {
    uint64_t block_size;
    uint64_t fragment_size;
    uint64_t blocks;
    uint64_t blocks_free;
    uint64_t blocks_available;
    uint64_t files;
    uint64_t files_free;
    uint64_t files_available;
    uint64_t filesystem_id;
    uint64_t flags;
    uint64_t name_max;
};

#define VFS_FS_RDONLY 1u
#define VFS_FS_NOSUID 2u

struct vfs_cred {
    uint32_t uid;
    uint32_t gid;
    uint32_t groups[VFS_GROUP_MAX];
    size_t group_count;
};

#define VFS_ACCESS_EXEC  1
#define VFS_ACCESS_WRITE 2
#define VFS_ACCESS_READ  4

enum vfs_node_type {
    VFS_NODE_REGULAR,
    VFS_NODE_DIRECTORY,
    VFS_NODE_SYMLINK,
    VFS_NODE_DEVICE,
};

struct vfs_timestamp {
    int64_t sec;
    int64_t nsec;
};

struct vfs_attr {
    uint64_t ino;
    enum vfs_node_type type;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    size_t size;
    struct vfs_timestamp atime, mtime, ctime;
};

#define VFS_SET_MODE  (1u << 0)
#define VFS_SET_UID   (1u << 1)
#define VFS_SET_GID   (1u << 2)
#define VFS_SET_ATIME (1u << 3)
#define VFS_SET_MTIME (1u << 4)

struct vfs_setattr {
    uint32_t valid;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    struct vfs_timestamp atime, mtime;
};

struct vfs_dirent {
    uint64_t ino;
    enum vfs_node_type type;
    char name[VFS_NAME_MAX + 1];
};

struct vfs_node;
struct vfs_dentry;
struct vfs_mount;

struct vfs_node_ops {
    long (*read)(struct vfs_node *, size_t, void *, size_t);
    long (*write)(struct vfs_node *, size_t, const void *, size_t);
    int (*truncate)(struct vfs_node *, size_t);
    int (*readdir)(struct vfs_node *, size_t, struct vfs_dirent *);
    int (*getattr)(struct vfs_node *, struct vfs_attr *);
    int (*setattr)(struct vfs_node *, const struct vfs_setattr *);
    long (*readlink)(struct vfs_node *, void *, size_t);
    /* Device-backed mmap: given an offset/length into this node, hand
     * back the EXISTING physical range to map (uint64_t rather than
     * phys_addr_t so this header stays free of a kernel/mm dependency —
     * callers already have phys_addr_t and it's the same width) and
     * whether it should be mapped cacheable. Real RAM (a future ramfs
     * file mapping) wants true; memory something outside the CPU reads
     * or writes continuously (the framebuffer today) wants false — see
     * VM_NOCACHE (kernel/include/kernel/mm/paging.h). Returns 0 with
     * the three out-parameters filled, or a negative errno. Nodes with
     * no memory to offer (ramfs files today, console, null, zero) leave
     * this unset; mmap() on those fails with -ENODEV. */
    int (*mmap)(struct vfs_node *, size_t offset, size_t length,
               uint64_t *out_phys, size_t *out_size, bool *out_cacheable);
    void (*destroy)(struct vfs_node *);
};

/* An inode-like filesystem object. Namespace entries retain it, and each
 * open-file description takes another reference. */
struct vfs_node {
    const struct vfs_node_ops *ops;
    void *private;
    enum vfs_node_type type;
    spinlock_t ref_lock;
    size_t refs;
    
    struct vfs_node *hash_next;
    uint64_t inode_num;
    struct vfs_mount *owner_mount;
};

/* A dentry is a name in one directory, deliberately separate from its node.
 * Multiple dentries can therefore name one inode once hard links arrive. */
struct vfs_dentry {
    struct vfs_node *node;
    struct vfs_dentry *parent;
    struct vfs_mount *owner_mount;
    void *private;
    spinlock_t ref_lock;
    size_t refs;
    int linked;
    char name[VFS_NAME_MAX + 1];
    
    struct vfs_dentry *hash_next;
};

struct vfs_path {
    struct vfs_mount *mount;
    struct vfs_dentry *dentry;
};

struct vfs_fs_ops {
    int (*root)(struct vfs_mount *, struct vfs_dentry **);
    int (*lookup_child)(struct vfs_mount *, struct vfs_dentry *,
                        const char *, struct vfs_dentry **);
    int (*create)(struct vfs_mount *, struct vfs_dentry *, const char *,
                  enum vfs_node_type, uint32_t, uint32_t, uint32_t,
                  struct vfs_dentry **);
    int (*remove)(struct vfs_mount *, struct vfs_dentry *, const char *, int);
    int (*rename)(struct vfs_mount *, struct vfs_dentry *, const char *,
                  struct vfs_dentry *, const char *);
    int (*link)(struct vfs_mount *, struct vfs_node *, struct vfs_dentry *,
                const char *, struct vfs_dentry **);
    int (*symlink)(struct vfs_mount *, struct vfs_dentry *, const char *,
                   const char *, uint32_t, uint32_t, struct vfs_dentry **);
    int (*statfs)(struct vfs_mount *, struct vfs_fs_info *);
    void (*destroy_dentry)(struct vfs_dentry *);
};

struct vfs_mount {
    const struct vfs_fs_ops *ops;
    void *private;
    struct vfs_dentry *root;
    struct vfs_path covered;
    int has_covered;
};

void vfs_node_init(struct vfs_node *node, const struct vfs_node_ops *ops,
                   void *private, enum vfs_node_type type);
void vfs_node_retain(struct vfs_node *node);
void vfs_node_release(struct vfs_node *node);
int vfs_dentry_init(struct vfs_dentry *dentry, struct vfs_node *node,
                    struct vfs_dentry *parent, const char *name, void *private);
void vfs_dentry_retain(struct vfs_dentry *dentry);
void vfs_dentry_release(struct vfs_dentry *dentry);
void vfs_path_retain(struct vfs_path *path);
void vfs_path_release(struct vfs_path *path);

struct vfs_node *vfs_cache_lookup_node(struct vfs_mount *mount, uint64_t ino);
void vfs_cache_insert_node(struct vfs_node *node);
struct vfs_dentry *vfs_cache_lookup_dentry(struct vfs_dentry *parent, const char *name);

void vfs_init(void);
int vfs_mount_root(const struct vfs_fs_ops *ops, void *private);
int vfs_mount_at(const struct vfs_path *cwd, const char *path,
                 const struct vfs_fs_ops *ops, void *private);
int vfs_root_path(struct vfs_path *out);
int vfs_lookup_path(const struct vfs_path *cwd, const char *path,
                    struct vfs_path *out);
int vfs_lookup_path_nofollow(const struct vfs_path *cwd, const char *path,
                             struct vfs_path *out);
int vfs_lookup_path_as(const struct vfs_path *cwd, const char *path,
                       int follow_final, const struct vfs_cred *cred,
                       struct vfs_path *out);
int vfs_get_path(const struct vfs_path *path, char *out, size_t out_size);
int vfs_check_access(struct vfs_node *node, const struct vfs_cred *cred,
                     int mode);

int vfs_open(const struct vfs_path *cwd, const char *path, int flags,
             uint32_t mode, const struct vfs_cred *cred,
             struct vfs_node **out, struct vfs_path *opened_path);
int vfs_mkdir(const struct vfs_path *cwd, const char *path, uint32_t mode,
              const struct vfs_cred *cred);
int vfs_unlink(const struct vfs_path *cwd, const char *path, int directory,
               const struct vfs_cred *cred);
int vfs_rename(const struct vfs_path *cwd, const char *old_path,
               const char *new_path, const struct vfs_cred *cred);
int vfs_rename_at(const struct vfs_path *old_base, const char *old_path,
                  const struct vfs_path *new_base, const char *new_path,
                  const struct vfs_cred *cred);
int vfs_link(const struct vfs_path *old_base, const char *old_path,
             const struct vfs_path *new_base, const char *new_path,
             int follow_source, const struct vfs_cred *cred);
int vfs_symlink(const struct vfs_path *cwd, const char *target,
                const char *link_path, const struct vfs_cred *cred);
long vfs_readlink(const struct vfs_path *cwd, const char *path,
                  void *buffer, size_t size, const struct vfs_cred *cred);
int vfs_stat(const struct vfs_path *cwd, const char *path,
             struct vfs_attr *attr, const struct vfs_cred *cred);
int vfs_lstat(const struct vfs_path *cwd, const char *path,
              struct vfs_attr *attr, const struct vfs_cred *cred);
int vfs_access_path(const struct vfs_path *cwd, const char *path, int mode,
                    int follow_final, const struct vfs_cred *cred);
int vfs_chmod_node(struct vfs_node *node, uint32_t mode,
                   const struct vfs_cred *cred);
int vfs_chown_node(struct vfs_node *node, uint32_t uid, uint32_t gid,
                   const struct vfs_cred *cred);
int vfs_utimens_node(struct vfs_node *node,
                     const struct vfs_timestamp *atime,
                     const struct vfs_timestamp *mtime, int explicit_times,
                     const struct vfs_cred *cred);
void vfs_clear_setid(struct vfs_node *node);

long vfs_read(struct vfs_node *node, size_t offset, void *buffer, size_t count);
long vfs_write(struct vfs_node *node, size_t offset,
               const void *buffer, size_t count);
int vfs_readdir(struct vfs_node *node, size_t index, struct vfs_dirent *entry);
int vfs_getattr(struct vfs_node *node, struct vfs_attr *attr);
int vfs_setattr(struct vfs_node *node, const struct vfs_setattr *attr);
int vfs_statfs_path(const struct vfs_path *path, struct vfs_fs_info *info);
int vfs_statfs_node(struct vfs_node *node, struct vfs_fs_info *info);

#endif
