#include <kernel/fs/vfs.h>
#include <kernel/fs/ramfs.h>
#include <kernel/errno.h>
#include <kernel/klibc/string.h>
#include <arch/irq_spinlock.h>
#include <stdbool.h>

#define O_ACCMODE  03
#define O_WRONLY   01
#define O_RDWR     02
#define O_CREAT    0100
#define O_EXCL     0200
#define O_TRUNC    01000
#define O_DIRECTORY 0200000

static struct vfs_mount mounts[VFS_MAX_MOUNTS];
static size_t mount_count;

void vfs_node_init(struct vfs_node *node, const struct vfs_node_ops *ops,
                   void *private, enum vfs_node_type type) {
    memset(node, 0, sizeof(*node));
    node->ops = ops;
    node->private = private;
    node->type = type;
    node->ref_lock = (spinlock_t)SPINLOCK_INIT;
}

void vfs_node_retain(struct vfs_node *node) {
    if (!node) return;
    irq_spin_lock(&node->ref_lock);
    node->refs++;
    irq_spin_unlock(&node->ref_lock);
}

void vfs_node_release(struct vfs_node *node) {
    if (!node) return;
    irq_spin_lock(&node->ref_lock);
    bool last = node->refs && --node->refs == 0;
    irq_spin_unlock(&node->ref_lock);
    if (last && node->ops && node->ops->destroy)
        node->ops->destroy(node);
}

int vfs_dentry_init(struct vfs_dentry *dentry, struct vfs_node *node,
                    struct vfs_dentry *parent, const char *name, void *private) {
    if (!dentry || !node || !name)
        return -EINVAL;
    size_t length = strlen(name);
    if (length > VFS_NAME_MAX || strchr(name, '/'))
        return -ENAMETOOLONG;
    memset(dentry, 0, sizeof(*dentry));
    dentry->node = node;
    dentry->parent = parent;
    dentry->private = private;
    dentry->ref_lock = (spinlock_t)SPINLOCK_INIT;
    dentry->refs = 1; /* namespace membership */
    dentry->linked = 1;
    memcpy(dentry->name, name, length + 1);
    vfs_node_retain(node);
    if (parent)
        vfs_dentry_retain(parent);
    return 0;
}

void vfs_dentry_retain(struct vfs_dentry *dentry) {
    if (!dentry) return;
    irq_spin_lock(&dentry->ref_lock);
    dentry->refs++;
    irq_spin_unlock(&dentry->ref_lock);
}

void vfs_dentry_release(struct vfs_dentry *dentry) {
    if (!dentry) return;
    irq_spin_lock(&dentry->ref_lock);
    bool last = dentry->refs && --dentry->refs == 0;
    irq_spin_unlock(&dentry->ref_lock);
    if (!last) return;
    struct vfs_node *node = dentry->node;
    struct vfs_dentry *parent = dentry->parent;
    struct vfs_mount *mount = dentry->owner_mount;
    if (mount && mount->ops && mount->ops->destroy_dentry)
        mount->ops->destroy_dentry(dentry);
    vfs_node_release(node);
    vfs_dentry_release(parent);
}

void vfs_path_retain(struct vfs_path *path) {
    if (path && path->dentry)
        vfs_dentry_retain(path->dentry);
}

void vfs_path_release(struct vfs_path *path) {
    if (!path) return;
    if (path->dentry)
        vfs_dentry_release(path->dentry);
    path->mount = NULL;
    path->dentry = NULL;
}

void vfs_init(void) {
    memset(mounts, 0, sizeof(mounts));
    mount_count = 0;
    ramfs_init();
}

int vfs_mount_root(const struct vfs_fs_ops *ops, void *private) {
    if (!ops || !ops->root || mount_count)
        return -EINVAL;
    struct vfs_mount *mount = &mounts[0];
    mount->ops = ops;
    mount->private = private;
    int result = ops->root(mount, &mount->root);
    if (result < 0 || !mount->root)
        return result < 0 ? result : -EINVAL;
    mount->root->owner_mount = mount;
    mount_count = 1;
    return 0;
}

int vfs_root_path(struct vfs_path *out) {
    if (!out || !mount_count)
        return -ENOENT;
    out->mount = &mounts[0];
    out->dentry = mounts[0].root;
    vfs_dentry_retain(out->dentry);
    return 0;
}

static int replace_path(struct vfs_path *path, struct vfs_mount *mount,
                        struct vfs_dentry *dentry) {
    if (!path || !mount || !dentry)
        return -EINVAL;
    vfs_dentry_retain(dentry);
    vfs_dentry_release(path->dentry);
    path->mount = mount;
    path->dentry = dentry;
    return 0;
}

static void follow_mount(struct vfs_path *path) {
    for (size_t i = 1; i < mount_count; i++) {
        struct vfs_mount *mounted = &mounts[i];
        if (mounted->has_covered
                && mounted->covered.mount == path->mount
                && mounted->covered.dentry == path->dentry) {
            replace_path(path, mounted, mounted->root);
            return;
        }
    }
}

static int step_parent(struct vfs_path *path) {
    if (path->dentry == path->mount->root && path->mount->has_covered) {
        struct vfs_path covered = path->mount->covered;
        replace_path(path, covered.mount, covered.dentry);
    }
    if (path->dentry->parent)
        replace_path(path, path->mount, path->dentry->parent);
    return 0;
}

static int walk(const struct vfs_path *cwd, const char *path, bool parent_only,
                struct vfs_path *out, char leaf[VFS_NAME_MAX + 1]) {
    if (!path || !out)
        return -EINVAL;
    size_t path_length = strlen(path);
    if (!path_length)
        return -ENOENT;
    if (path_length > VFS_PATH_MAX)
        return -ENAMETOOLONG;

    struct vfs_path current = {0};
    int result;
    if (path[0] == '/' || !cwd || !cwd->dentry)
        result = vfs_root_path(&current);
    else {
        current = *cwd;
        vfs_path_retain(&current);
        result = 0;
    }
    if (result < 0)
        return result;

    const char *cursor = path;
    for (;;) {
        while (*cursor == '/') cursor++;
        if (!*cursor) {
            if (parent_only) {
                vfs_path_release(&current);
                return -EINVAL;
            }
            *out = current;
            return 0;
        }
        const char *start = cursor;
        while (*cursor && *cursor != '/') cursor++;
        size_t length = (size_t)(cursor - start);
        if (length > VFS_NAME_MAX) {
            vfs_path_release(&current);
            return -ENAMETOOLONG;
        }
        const char *after = cursor;
        while (*after == '/') after++;
        bool final = !*after;

        if (parent_only && final) {
            if ((length == 1 && start[0] == '.')
                    || (length == 2 && start[0] == '.' && start[1] == '.')) {
                vfs_path_release(&current);
                return -EINVAL;
            }
            memcpy(leaf, start, length);
            leaf[length] = '\0';
            *out = current;
            return 0;
        }

        if (length == 1 && start[0] == '.') {
            cursor = after;
            continue;
        }
        if (length == 2 && start[0] == '.' && start[1] == '.') {
            step_parent(&current);
            cursor = after;
            continue;
        }
        if (!current.dentry->node
                || current.dentry->node->type != VFS_NODE_DIRECTORY) {
            vfs_path_release(&current);
            return -ENOTDIR;
        }
        char component[VFS_NAME_MAX + 1];
        memcpy(component, start, length);
        component[length] = '\0';
        struct vfs_dentry *child = NULL;
        result = current.mount->ops->lookup_child(
            current.mount, current.dentry, component, &child);
        if (result < 0) {
            vfs_path_release(&current);
            return result;
        }
        child->owner_mount = current.mount;
        vfs_dentry_release(current.dentry);
        current.dentry = child; /* lookup_child returned a retained dentry */
        follow_mount(&current);
        cursor = after;
    }
}

int vfs_lookup_path(const struct vfs_path *cwd, const char *path,
                    struct vfs_path *out) {
    int result = walk(cwd, path, false, out, NULL);
    if (result < 0)
        return result;
    size_t length = strlen(path);
    if (length && path[length - 1] == '/'
            && out->dentry->node->type != VFS_NODE_DIRECTORY) {
        vfs_path_release(out);
        return -ENOTDIR;
    }
    return 0;
}

int vfs_mount_at(const struct vfs_path *cwd, const char *path,
                 const struct vfs_fs_ops *ops, void *private) {
    if (!ops || !ops->root || mount_count >= VFS_MAX_MOUNTS)
        return -ENOSPC;
    struct vfs_path covered;
    int result = vfs_lookup_path(cwd, path, &covered);
    if (result < 0)
        return result;
    if (covered.dentry->node->type != VFS_NODE_DIRECTORY) {
        vfs_path_release(&covered);
        return -ENOTDIR;
    }
    for (size_t i = 1; i < mount_count; i++)
        if (mounts[i].covered.mount == covered.mount
                && mounts[i].covered.dentry == covered.dentry) {
            vfs_path_release(&covered);
            return -EEXIST;
        }
    struct vfs_mount *mount = &mounts[mount_count];
    memset(mount, 0, sizeof(*mount));
    mount->ops = ops;
    mount->private = private;
    mount->covered = covered;
    mount->has_covered = 1;
    result = ops->root(mount, &mount->root);
    if (result < 0 || !mount->root) {
        vfs_path_release(&mount->covered);
        memset(mount, 0, sizeof(*mount));
        return result < 0 ? result : -EINVAL;
    }
    mount->root->owner_mount = mount;
    mount_count++;
    return 0;
}

int vfs_get_path(const struct vfs_path *path, char *out, size_t out_size) {
    if (!path || !path->mount || !path->dentry || !out || !out_size)
        return -EINVAL;
    char reverse[VFS_PATH_MAX + 1];
    size_t begin = sizeof(reverse);
    struct vfs_mount *mount = path->mount;
    struct vfs_dentry *dentry = path->dentry;

    while (!(mount == &mounts[0] && dentry == mounts[0].root)) {
        if (!dentry->linked)
            return -ENOENT;
        if (dentry == mount->root && mount->has_covered) {
            dentry = mount->covered.dentry;
            mount = mount->covered.mount;
        }
        size_t length = strlen(dentry->name);
        if (!length || begin < length + 1)
            return -ENAMETOOLONG;
        begin -= length;
        memcpy(reverse + begin, dentry->name, length);
        reverse[--begin] = '/';
        if (dentry->parent)
            dentry = dentry->parent;
        else if (!(mount == &mounts[0] && dentry == mounts[0].root))
            return -ENOENT;
    }

    if (begin == sizeof(reverse))
        reverse[--begin] = '/';
    size_t length = sizeof(reverse) - begin;
    if (length + 1 > out_size)
        return -ENAMETOOLONG;
    memcpy(out, reverse + begin, length);
    out[length] = '\0';
    return 0;
}

int vfs_open(const struct vfs_path *cwd, const char *path, int flags,
             uint32_t mode, struct vfs_node **out) {
    if (!path || !out)
        return -EINVAL;
    int access = flags & O_ACCMODE;
    if (access != 0 && access != O_WRONLY && access != O_RDWR)
        return -EINVAL;
    struct vfs_path found;
    int result = vfs_lookup_path(cwd, path, &found);
    if (result == 0) {
        struct vfs_node *node = found.dentry->node;
        if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL))
            result = -EEXIST;
        else if ((flags & O_DIRECTORY) && node->type != VFS_NODE_DIRECTORY)
            result = -ENOTDIR;
        else if (node->type == VFS_NODE_DIRECTORY && access != 0)
            result = -EISDIR;
        else if ((flags & O_TRUNC) && access != 0) {
            result = node->ops && node->ops->truncate
                ? node->ops->truncate(node, 0) : -EINVAL;
        }
        if (result == 0) {
            vfs_node_retain(node);
            *out = node;
        }
        vfs_path_release(&found);
        return result;
    }
    if (result != -ENOENT || !(flags & O_CREAT))
        return result;
    size_t path_length = strlen(path);
    if (path_length && path[path_length - 1] == '/')
        return -ENOENT;

    struct vfs_path parent;
    char leaf[VFS_NAME_MAX + 1];
    result = walk(cwd, path, true, &parent, leaf);
    if (result < 0)
        return result;
    if (parent.dentry->node->type != VFS_NODE_DIRECTORY) {
        vfs_path_release(&parent);
        return -ENOTDIR;
    }
    struct vfs_dentry *created = NULL;
    result = parent.mount->ops->create(parent.mount, parent.dentry, leaf,
                                       VFS_NODE_REGULAR, mode, &created);
    if (result == 0) {
        created->owner_mount = parent.mount;
        vfs_node_retain(created->node);
        *out = created->node;
        vfs_dentry_release(created);
    }
    vfs_path_release(&parent);
    return result;
}

int vfs_mkdir(const struct vfs_path *cwd, const char *path, uint32_t mode) {
    struct vfs_path parent;
    char leaf[VFS_NAME_MAX + 1];
    int result = walk(cwd, path, true, &parent, leaf);
    if (result < 0)
        return result;
    if (parent.dentry->node->type != VFS_NODE_DIRECTORY) {
        vfs_path_release(&parent);
        return -ENOTDIR;
    }
    struct vfs_dentry *created = NULL;
    result = parent.mount->ops->create(parent.mount, parent.dentry, leaf,
                                       VFS_NODE_DIRECTORY, mode, &created);
    if (result == 0) {
        created->owner_mount = parent.mount;
        vfs_dentry_release(created);
    }
    vfs_path_release(&parent);
    return result;
}

static bool vfs_is_mountpoint(struct vfs_mount *mount,
                              struct vfs_dentry *dentry) {
    for (size_t i = 1; i < mount_count; i++)
        if (mounts[i].has_covered
                && mounts[i].covered.mount == mount
                && mounts[i].covered.dentry == dentry)
            return true;
    return false;
}

static bool path_is_root_syntax(const char *path) {
    if (!path || *path != '/')
        return false;
    while (*path == '/')
        path++;
    return !*path;
}

int vfs_unlink(const struct vfs_path *cwd, const char *path, int directory) {
    if (!path)
        return -EINVAL;
    if (path_is_root_syntax(path))
        return directory ? -EBUSY : -EISDIR;
    struct vfs_path parent;
    char leaf[VFS_NAME_MAX + 1];
    int result = walk(cwd, path, true, &parent, leaf);
    if (result < 0)
        return result;
    if (!parent.mount->ops->remove) {
        vfs_path_release(&parent);
        return -EROFS;
    }

    struct vfs_dentry *target = NULL;
    result = parent.mount->ops->lookup_child(parent.mount, parent.dentry,
                                              leaf, &target);
    if (result < 0) {
        vfs_path_release(&parent);
        return result;
    }
    size_t length = strlen(path);
    if (length && path[length - 1] == '/'
            && target->node->type != VFS_NODE_DIRECTORY)
        result = -ENOTDIR;
    else if (vfs_is_mountpoint(parent.mount, target))
        result = -EBUSY;
    vfs_dentry_release(target);
    if (result == 0)
        result = parent.mount->ops->remove(parent.mount, parent.dentry,
                                            leaf, directory);
    vfs_path_release(&parent);
    return result;
}

int vfs_rename(const struct vfs_path *cwd, const char *old_path,
               const char *new_path) {
    if (!old_path || !new_path)
        return -EINVAL;
    if (path_is_root_syntax(old_path) || path_is_root_syntax(new_path))
        return -EBUSY;
    struct vfs_path old_parent, new_parent;
    char old_leaf[VFS_NAME_MAX + 1], new_leaf[VFS_NAME_MAX + 1];
    int result = walk(cwd, old_path, true, &old_parent, old_leaf);
    if (result < 0)
        return result;
    result = walk(cwd, new_path, true, &new_parent, new_leaf);
    if (result < 0) {
        vfs_path_release(&old_parent);
        return result;
    }
    if (old_parent.mount != new_parent.mount) {
        result = -EXDEV;
        goto out;
    }
    if (!old_parent.mount->ops->rename) {
        result = -EROFS;
        goto out;
    }

    struct vfs_dentry *source = NULL;
    result = old_parent.mount->ops->lookup_child(old_parent.mount,
                                                  old_parent.dentry,
                                                  old_leaf, &source);
    if (result < 0)
        goto out;
    enum vfs_node_type source_type = source->node->type;
    size_t old_length = strlen(old_path);
    if (old_length && old_path[old_length - 1] == '/'
            && source->node->type != VFS_NODE_DIRECTORY)
        result = -ENOTDIR;
    else if (vfs_is_mountpoint(old_parent.mount, source))
        result = -EBUSY;
    vfs_dentry_release(source);
    if (result < 0)
        goto out;

    struct vfs_dentry *destination = NULL;
    int destination_result = new_parent.mount->ops->lookup_child(
        new_parent.mount, new_parent.dentry, new_leaf, &destination);
    if (destination_result == 0) {
        size_t new_length = strlen(new_path);
        if (new_length && new_path[new_length - 1] == '/'
                && destination->node->type != VFS_NODE_DIRECTORY)
            result = -ENOTDIR;
        else if (vfs_is_mountpoint(new_parent.mount, destination))
            result = -EBUSY;
        vfs_dentry_release(destination);
        if (result < 0)
            goto out;
    } else if (destination_result != -ENOENT) {
        result = destination_result;
        goto out;
    } else {
        size_t new_length = strlen(new_path);
        if (new_length && new_path[new_length - 1] == '/') {
            result = source_type == VFS_NODE_DIRECTORY ? -ENOENT : -ENOTDIR;
            goto out;
        }
    }
    result = old_parent.mount->ops->rename(old_parent.mount,
        old_parent.dentry, old_leaf, new_parent.dentry, new_leaf);

out:
    vfs_path_release(&new_parent);
    vfs_path_release(&old_parent);
    return result;
}

int vfs_stat(const struct vfs_path *cwd, const char *path,
             struct vfs_attr *attr) {
    if (!attr)
        return -EINVAL;
    struct vfs_path found;
    int result = vfs_lookup_path(cwd, path, &found);
    if (result < 0)
        return result;
    result = vfs_getattr(found.dentry->node, attr);
    vfs_path_release(&found);
    return result;
}

long vfs_read(struct vfs_node *node, size_t offset, void *buffer, size_t count) {
    if (!node || !node->ops || !node->ops->read)
        return -EINVAL;
    return node->ops->read(node, offset, buffer, count);
}

long vfs_write(struct vfs_node *node, size_t offset,
               const void *buffer, size_t count) {
    if (!node || !node->ops || !node->ops->write)
        return -EINVAL;
    return node->ops->write(node, offset, buffer, count);
}

int vfs_readdir(struct vfs_node *node, size_t index, struct vfs_dirent *entry) {
    if (!node || !node->ops || !node->ops->readdir || !entry)
        return -EINVAL;
    return node->ops->readdir(node, index, entry);
}

int vfs_getattr(struct vfs_node *node, struct vfs_attr *attr) {
    if (!node || !node->ops || !node->ops->getattr || !attr)
        return -EINVAL;
    return node->ops->getattr(node, attr);
}
