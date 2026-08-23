#include <kernel/fs/vfs.h>
#include <kernel/fs/ramfs.h>

/* One root mount is enough to establish the VFS/filesystem boundary. A
 * mount table and longest-prefix routing can be added here without changing
 * descriptors or syscalls once a second filesystem (devfs) arrives. */
static struct vfs_mount root_mount;
static int have_root;

void vfs_init(void) {
    have_root = 0;
    ramfs_init();
}

int vfs_mount_root(const struct vfs_fs_ops *ops, void *private) {
    if (!ops || have_root)
        return -1;
    root_mount.ops = ops;
    root_mount.private = private;
    have_root = 1;
    return 0;
}

int vfs_open(const char *cwd, const char *path, int flags,
             struct vfs_node **out) {
    if (!have_root || !root_mount.ops->open || !path || !out)
        return -1;
    int result = root_mount.ops->open(&root_mount, cwd, path, flags, out);
    if (result == 0)
        (*out)->mount = &root_mount;
    return result;
}

int vfs_lookup(const char *cwd, const char *path, struct vfs_node **out) {
    if (!have_root || !root_mount.ops->lookup || !path || !out)
        return -1;
    int result = root_mount.ops->lookup(&root_mount, cwd, path, out);
    if (result == 0)
        (*out)->mount = &root_mount;
    return result;
}

int vfs_mkdir(const char *cwd, const char *path, uint32_t mode) {
    if (!have_root || !root_mount.ops->mkdir || !path)
        return -1;
    return root_mount.ops->mkdir(&root_mount, cwd, path, mode);
}

int vfs_resolve_directory(const char *cwd, const char *path,
                          char *out, size_t out_size) {
    if (!have_root || !root_mount.ops->resolve_directory || !path || !out)
        return -1;
    return root_mount.ops->resolve_directory(&root_mount, cwd, path,
                                              out, out_size);
}

int vfs_stat(const char *cwd, const char *path, struct vfs_attr *attr) {
    struct vfs_node *node;
    if (!attr || vfs_lookup(cwd, path, &node) != 0)
        return -1;
    return vfs_getattr(node, attr);
}

long vfs_read(struct vfs_node *node, size_t offset, void *buffer, size_t count) {
    if (!node || !node->ops || !node->ops->read)
        return -1;
    return node->ops->read(node, offset, buffer, count);
}

long vfs_write(struct vfs_node *node, size_t offset,
               const void *buffer, size_t count) {
    if (!node || !node->ops || !node->ops->write)
        return -1;
    return node->ops->write(node, offset, buffer, count);
}

int vfs_readdir(struct vfs_node *node, size_t index, struct vfs_dirent *entry) {
    if (!node || !node->ops || !node->ops->readdir || !entry)
        return -1;
    return node->ops->readdir(node, index, entry);
}

int vfs_getattr(struct vfs_node *node, struct vfs_attr *attr) {
    if (!node || !node->ops || !node->ops->getattr || !attr)
        return -1;
    return node->ops->getattr(node, attr);
}
