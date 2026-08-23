#ifndef KERNEL_FS_VFS_H
#define KERNEL_FS_VFS_H

#include <stddef.h>
#include <stdint.h>

#define VFS_PATH_MAX 100

enum vfs_node_type {
    VFS_NODE_REGULAR,
    VFS_NODE_DIRECTORY,
};

struct vfs_attr {
    uint64_t ino;
    enum vfs_node_type type;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    size_t size;
};

struct vfs_dirent {
    uint64_t ino;
    enum vfs_node_type type;
    char name[VFS_PATH_MAX + 1];
};

struct vfs_node;
struct vfs_mount;

struct vfs_node_ops {
    long (*read)(struct vfs_node *, size_t, void *, size_t);
    long (*write)(struct vfs_node *, size_t, const void *, size_t);
    int (*readdir)(struct vfs_node *, size_t, struct vfs_dirent *);
    int (*getattr)(struct vfs_node *, struct vfs_attr *);
};

struct vfs_node {
    const struct vfs_node_ops *ops;
    struct vfs_mount *mount;
    void *private;
    enum vfs_node_type type;
};

struct vfs_fs_ops {
    int (*open)(struct vfs_mount *, const char *, const char *, int,
                struct vfs_node **);
    int (*lookup)(struct vfs_mount *, const char *, const char *,
                  struct vfs_node **);
    int (*mkdir)(struct vfs_mount *, const char *, const char *, uint32_t);
    int (*resolve_directory)(struct vfs_mount *, const char *, const char *,
                             char *, size_t);
};

struct vfs_mount {
    const struct vfs_fs_ops *ops;
    void *private;
};

void vfs_init(void);
int vfs_mount_root(const struct vfs_fs_ops *ops, void *private);

int vfs_open(const char *cwd, const char *path, int flags,
             struct vfs_node **out);
int vfs_lookup(const char *cwd, const char *path, struct vfs_node **out);
int vfs_mkdir(const char *cwd, const char *path, uint32_t mode);
int vfs_resolve_directory(const char *cwd, const char *path,
                          char *out, size_t out_size);
int vfs_stat(const char *cwd, const char *path, struct vfs_attr *attr);

long vfs_read(struct vfs_node *node, size_t offset, void *buffer, size_t count);
long vfs_write(struct vfs_node *node, size_t offset,
               const void *buffer, size_t count);
int vfs_readdir(struct vfs_node *node, size_t index, struct vfs_dirent *entry);
int vfs_getattr(struct vfs_node *node, struct vfs_attr *attr);

#endif
