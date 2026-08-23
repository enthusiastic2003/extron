#include <kernel/fs/ramfs.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/tar.h>
#include <kernel/errno.h>
#include <kernel/mm/kheap.h>
#include <kernel/klibc/string.h>
#include <arch/irq_spinlock.h>

struct ramfs_dentry;

struct ramfs_inode {
    struct vfs_node vnode;
    spinlock_t lock;
    bool owns_data;
    uint8_t *data;
    size_t size;
    size_t capacity;
    uint64_t ino;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    struct ramfs_dentry *children;
};

struct ramfs_dentry {
    struct vfs_dentry dentry;
    struct ramfs_dentry *next_sibling;
};

static struct ramfs_inode root_inode;
static struct ramfs_dentry root_dentry;
static spinlock_t tree_lock = SPINLOCK_INIT;
static uint64_t next_ino;
static const struct vfs_fs_ops ramfs_fs_ops;
static struct ramfs_dentry **child_link_locked(struct ramfs_dentry *,
                                                struct ramfs_dentry *);

static struct ramfs_inode *inode_of(struct vfs_node *node) {
    return node ? node->private : NULL;
}

static struct ramfs_dentry *dentry_of(struct vfs_dentry *dentry) {
    return dentry ? dentry->private : NULL;
}

static long ramfs_node_read(struct vfs_node *, size_t, void *, size_t);
static long ramfs_node_write(struct vfs_node *, size_t, const void *, size_t);
static int ramfs_node_truncate(struct vfs_node *, size_t);
static int ramfs_node_readdir(struct vfs_node *, size_t, struct vfs_dirent *);
static int ramfs_node_getattr(struct vfs_node *, struct vfs_attr *);
static long ramfs_node_readlink(struct vfs_node *, void *, size_t);
static void ramfs_node_destroy(struct vfs_node *);

static const struct vfs_node_ops ramfs_node_ops = {
    .read = ramfs_node_read,
    .write = ramfs_node_write,
    .truncate = ramfs_node_truncate,
    .readdir = ramfs_node_readdir,
    .getattr = ramfs_node_getattr,
    .readlink = ramfs_node_readlink,
    .destroy = ramfs_node_destroy,
};

static struct ramfs_dentry *find_child_locked(struct ramfs_dentry *parent,
                                               const char *name) {
    struct ramfs_inode *parent_inode = inode_of(parent->dentry.node);
    for (struct ramfs_dentry *child = parent_inode->children;
         child; child = child->next_sibling)
        if (strcmp(child->dentry.name, name) == 0)
            return child;
    return NULL;
}

static struct ramfs_inode *new_inode(enum vfs_node_type type, uint32_t mode) {
    struct ramfs_inode *inode = kmalloc(sizeof(*inode));
    if (!inode) return NULL;
    memset(inode, 0, sizeof(*inode));
    vfs_node_init(&inode->vnode, &ramfs_node_ops, inode, type);
    inode->lock = (spinlock_t)SPINLOCK_INIT;
    inode->ino = next_ino++;
    inode->mode = mode & 07777;
    inode->nlink = type == VFS_NODE_DIRECTORY ? 2 : 1;
    return inode;
}

static int create_locked(struct ramfs_dentry *parent, const char *name,
                         enum vfs_node_type type, uint32_t mode,
                         struct vfs_dentry **out) {
    if (!parent || !name || !*name)
        return -EINVAL;
    if (parent->dentry.node->type != VFS_NODE_DIRECTORY)
        return -ENOTDIR;
    if (find_child_locked(parent, name))
        return -EEXIST;

    struct ramfs_inode *inode = new_inode(type, mode);
    struct ramfs_dentry *dentry = kmalloc(sizeof(*dentry));
    if (!inode || !dentry) {
        if (inode) kfree(inode);
        if (dentry) kfree(dentry);
        return -ENOMEM;
    }
    memset(dentry, 0, sizeof(*dentry));
    int result = vfs_dentry_init(&dentry->dentry, &inode->vnode,
                                 &parent->dentry, name, dentry);
    if (result < 0) {
        kfree(dentry);
        kfree(inode);
        return result;
    }
    struct ramfs_inode *parent_inode = inode_of(parent->dentry.node);
    dentry->next_sibling = parent_inode->children;
    parent_inode->children = dentry;
    if (type == VFS_NODE_DIRECTORY) {
        parent_inode->nlink++;
    }
    if (out) {
        vfs_dentry_retain(&dentry->dentry);
        *out = &dentry->dentry;
    }
    return 0;
}

static int ramfs_root(struct vfs_mount *mount, struct vfs_dentry **out) {
    (void)mount;
    if (!out) return -EINVAL;
    vfs_dentry_retain(&root_dentry.dentry);
    *out = &root_dentry.dentry;
    return 0;
}

static int ramfs_lookup_child(struct vfs_mount *mount,
                              struct vfs_dentry *parent_entry,
                              const char *name, struct vfs_dentry **out) {
    (void)mount;
    if (!parent_entry || !name || !out)
        return -EINVAL;
    if (parent_entry->node->type != VFS_NODE_DIRECTORY)
        return -ENOTDIR;
    struct ramfs_dentry *parent = dentry_of(parent_entry);
    irq_spin_lock(&tree_lock);
    struct ramfs_dentry *child = find_child_locked(parent, name);
    if (child)
        vfs_dentry_retain(&child->dentry);
    irq_spin_unlock(&tree_lock);
    if (!child)
        return -ENOENT;
    *out = &child->dentry;
    return 0;
}

static int ramfs_create(struct vfs_mount *mount,
                        struct vfs_dentry *parent_entry, const char *name,
                        enum vfs_node_type type, uint32_t mode,
                        struct vfs_dentry **out) {
    (void)mount;
    struct ramfs_dentry *parent = dentry_of(parent_entry);
    if (!parent)
        return -EINVAL;
    irq_spin_lock(&tree_lock);
    int result = create_locked(parent, name, type, mode, out);
    irq_spin_unlock(&tree_lock);
    return result;
}

static int ramfs_link(struct vfs_mount *mount, struct vfs_node *node,
                      struct vfs_dentry *parent_entry, const char *name,
                      struct vfs_dentry **out) {
    (void)mount;
    if (!node || node->type == VFS_NODE_DIRECTORY)
        return -EPERM;
    struct ramfs_dentry *parent = dentry_of(parent_entry);
    irq_spin_lock(&tree_lock);
    if (find_child_locked(parent, name)) {
        irq_spin_unlock(&tree_lock);
        return -EEXIST;
    }
    struct ramfs_dentry *entry = kmalloc(sizeof(*entry));
    if (!entry) {
        irq_spin_unlock(&tree_lock);
        return -ENOMEM;
    }
    memset(entry, 0, sizeof(*entry));
    int result = vfs_dentry_init(&entry->dentry, node, parent_entry,
                                  name, entry);
    if (result == 0) {
        struct ramfs_inode *parent_inode = inode_of(parent_entry->node);
        entry->next_sibling = parent_inode->children;
        parent_inode->children = entry;
        inode_of(node)->nlink++;
        vfs_dentry_retain(&entry->dentry);
        *out = &entry->dentry;
    } else {
        kfree(entry);
    }
    irq_spin_unlock(&tree_lock);
    return result;
}

static int ramfs_symlink(struct vfs_mount *mount,
                         struct vfs_dentry *parent_entry, const char *name,
                         const char *target, struct vfs_dentry **out) {
    (void)mount;
    size_t length = strlen(target);
    if (length > VFS_PATH_MAX)
        return -ENAMETOOLONG;
    struct ramfs_dentry *parent = dentry_of(parent_entry);
    irq_spin_lock(&tree_lock);
    struct vfs_dentry *created = NULL;
    int result = create_locked(parent, name, VFS_NODE_SYMLINK, 0777, &created);
    if (result == 0) {
        struct ramfs_inode *inode = inode_of(created->node);
        inode->data = length ? kmalloc(length) : NULL;
        if (length && !inode->data) {
            /* Creation rollback is not observable while tree_lock is held. */
            struct ramfs_dentry *entry = dentry_of(created);
            struct ramfs_dentry **link = child_link_locked(parent, entry);
            *link = entry->next_sibling;
            entry->dentry.linked = 0;
            vfs_dentry_release(created);
            vfs_dentry_release(&entry->dentry);
            result = -ENOMEM;
        } else {
            if (length) memcpy(inode->data, target, length);
            inode->size = inode->capacity = length;
            inode->owns_data = true;
            *out = created;
        }
    }
    irq_spin_unlock(&tree_lock);
    return result;
}

static struct ramfs_dentry **child_link_locked(struct ramfs_dentry *parent,
                                                struct ramfs_dentry *child) {
    struct ramfs_dentry **link = &inode_of(parent->dentry.node)->children;
    while (*link && *link != child)
        link = &(*link)->next_sibling;
    return *link ? link : NULL;
}

static int ramfs_remove(struct vfs_mount *mount,
                        struct vfs_dentry *parent_entry, const char *name,
                        int directory) {
    (void)mount;
    struct ramfs_dentry *parent = dentry_of(parent_entry);
    if (!parent || !name || !*name)
        return -EINVAL;
    irq_spin_lock(&tree_lock);
    struct ramfs_dentry *child = find_child_locked(parent, name);
    if (!child) {
        irq_spin_unlock(&tree_lock);
        return -ENOENT;
    }
    bool is_directory = child->dentry.node->type == VFS_NODE_DIRECTORY;
    if (directory && !is_directory) {
        irq_spin_unlock(&tree_lock);
        return -ENOTDIR;
    }
    if (!directory && is_directory) {
        irq_spin_unlock(&tree_lock);
        return -EISDIR;
    }
    if (is_directory && inode_of(child->dentry.node)->children) {
        irq_spin_unlock(&tree_lock);
        return -ENOTEMPTY;
    }
    struct ramfs_dentry **link = child_link_locked(parent, child);
    if (!link) {
        irq_spin_unlock(&tree_lock);
        return -ENOENT;
    }
    *link = child->next_sibling;
    child->next_sibling = NULL;
    child->dentry.linked = 0;
    struct ramfs_inode *inode = inode_of(child->dentry.node);
    if (is_directory) {
        inode_of(parent_entry->node)->nlink--;
        inode->nlink = 0;
    } else if (inode->nlink) {
        inode->nlink--;
    }
    irq_spin_unlock(&tree_lock);

    /* Drop namespace membership. Open files and cwd paths keep their own
     * references, so the inode/dentry can outlive its visible name. */
    vfs_dentry_release(&child->dentry);
    return 0;
}

static int ramfs_rename(struct vfs_mount *mount,
                        struct vfs_dentry *old_parent_entry,
                        const char *old_name,
                        struct vfs_dentry *new_parent_entry,
                        const char *new_name) {
    (void)mount;
    struct ramfs_dentry *old_parent = dentry_of(old_parent_entry);
    struct ramfs_dentry *new_parent = dentry_of(new_parent_entry);
    if (!old_parent || !new_parent || !old_name || !*old_name
            || !new_name || !*new_name)
        return -EINVAL;

    struct ramfs_dentry *replaced = NULL;
    struct vfs_dentry *old_parent_ref = NULL;
    irq_spin_lock(&tree_lock);
    struct ramfs_dentry *source = find_child_locked(old_parent, old_name);
    if (!source) {
        irq_spin_unlock(&tree_lock);
        return -ENOENT;
    }
    struct ramfs_dentry *target = find_child_locked(new_parent, new_name);
    if (source == target) {
        irq_spin_unlock(&tree_lock);
        return 0;
    }

    bool source_directory = source->dentry.node->type == VFS_NODE_DIRECTORY;
    if (source_directory) {
        for (struct vfs_dentry *ancestor = new_parent_entry;
             ancestor; ancestor = ancestor->parent) {
            if (ancestor == &source->dentry) {
                irq_spin_unlock(&tree_lock);
                return -EINVAL;
            }
        }
    }
    if (target) {
        bool target_directory = target->dentry.node->type == VFS_NODE_DIRECTORY;
        if (source_directory && !target_directory) {
            irq_spin_unlock(&tree_lock);
            return -ENOTDIR;
        }
        if (!source_directory && target_directory) {
            irq_spin_unlock(&tree_lock);
            return -EISDIR;
        }
        if (target_directory && inode_of(target->dentry.node)->children) {
            irq_spin_unlock(&tree_lock);
            return -ENOTEMPTY;
        }
    }

    if (target) {
        struct ramfs_dentry **target_link = child_link_locked(new_parent,
                                                               target);
        *target_link = target->next_sibling;
        target->next_sibling = NULL;
        target->dentry.linked = 0;
        struct ramfs_inode *target_inode = inode_of(target->dentry.node);
        if (target->dentry.node->type == VFS_NODE_DIRECTORY) {
            inode_of(new_parent_entry->node)->nlink--;
            target_inode->nlink = 0;
        } else if (target_inode->nlink) {
            target_inode->nlink--;
        }
        replaced = target;
    }

    struct ramfs_dentry **source_link = child_link_locked(old_parent, source);
    *source_link = source->next_sibling;
    if (old_parent != new_parent) {
        vfs_dentry_retain(new_parent_entry);
        old_parent_ref = source->dentry.parent;
        source->dentry.parent = new_parent_entry;
        if (source_directory) {
            inode_of(old_parent_entry->node)->nlink--;
            inode_of(new_parent_entry->node)->nlink++;
        }
    }
    memcpy(source->dentry.name, new_name, strlen(new_name) + 1);
    struct ramfs_inode *new_parent_inode = inode_of(new_parent_entry->node);
    source->next_sibling = new_parent_inode->children;
    new_parent_inode->children = source;
    irq_spin_unlock(&tree_lock);

    if (old_parent_ref)
        vfs_dentry_release(old_parent_ref);
    if (replaced)
        vfs_dentry_release(&replaced->dentry);
    return 0;
}

static void seed_tar_file(const struct tar_file *file, void *context) {
    (void)context;
    const char *cursor = file->name;
    while (*cursor == '/') cursor++;
    while (cursor[0] == '.' && cursor[1] == '/') cursor += 2;
    if (!*cursor) return;

    struct ramfs_dentry *parent = &root_dentry;
    irq_spin_lock(&tree_lock);
    for (;;) {
        const char *start = cursor;
        while (*cursor && *cursor != '/') cursor++;
        size_t length = (size_t)(cursor - start);
        while (*cursor == '/') cursor++;
        if (!length || length > VFS_NAME_MAX) {
            irq_spin_unlock(&tree_lock);
            return;
        }
        char name[VFS_NAME_MAX + 1];
        memcpy(name, start, length);
        name[length] = '\0';
        bool final = !*cursor;
        struct ramfs_dentry *child = find_child_locked(parent, name);
        if (!child) {
            struct vfs_dentry *created = NULL;
            int result = create_locked(parent, name,
                final ? VFS_NODE_REGULAR : VFS_NODE_DIRECTORY,
                final ? 0644 : 0755, &created);
            if (result < 0) {
                irq_spin_unlock(&tree_lock);
                return;
            }
            child = dentry_of(created);
            vfs_dentry_release(created);
        }
        if (final) {
            struct ramfs_inode *inode = inode_of(child->dentry.node);
            if (inode && child->dentry.node->type == VFS_NODE_REGULAR) {
                inode->data = file->data;
                inode->size = inode->capacity = file->size;
                inode->owns_data = false;
            }
            irq_spin_unlock(&tree_lock);
            return;
        }
        if (child->dentry.node->type != VFS_NODE_DIRECTORY) {
            irq_spin_unlock(&tree_lock);
            return;
        }
        parent = child;
    }
}

void ramfs_init(void) {
    memset(&root_inode, 0, sizeof(root_inode));
    memset(&root_dentry, 0, sizeof(root_dentry));
    vfs_node_init(&root_inode.vnode, &ramfs_node_ops, &root_inode,
                  VFS_NODE_DIRECTORY);
    root_inode.lock = (spinlock_t)SPINLOCK_INIT;
    root_inode.ino = 1;
    root_inode.mode = 0755;
    root_inode.nlink = 2;
    next_ino = 2;
    if (vfs_dentry_init(&root_dentry.dentry, &root_inode.vnode,
                        NULL, "", &root_dentry) < 0)
        return;
    tar_foreach(seed_tar_file, NULL);
    vfs_mount_root(&ramfs_fs_ops, NULL);
}

static int ensure_capacity_locked(struct ramfs_inode *inode, size_t size) {
    if (inode->owns_data && size <= inode->capacity)
        return 0;
    size_t capacity = inode->capacity ? inode->capacity : 256;
    while (capacity < size) {
        if (capacity > (size_t)-1 / 2) {
            capacity = size;
            break;
        }
        capacity *= 2;
    }
    uint8_t *data = capacity ? kmalloc(capacity) : NULL;
    if (capacity && !data)
        return -ENOMEM;
    if (capacity) memset(data, 0, capacity);
    if (inode->size) memcpy(data, inode->data, inode->size);
    if (inode->owns_data) kfree(inode->data);
    inode->data = data;
    inode->capacity = capacity;
    inode->owns_data = true;
    return 0;
}

static long ramfs_node_read(struct vfs_node *vnode, size_t offset,
                            void *buffer, size_t count) {
    struct ramfs_inode *inode = inode_of(vnode);
    if (!inode) return -EINVAL;
    if (vnode->type != VFS_NODE_REGULAR) return -EISDIR;
    irq_spin_lock(&inode->lock);
    if (offset > inode->size) offset = inode->size;
    if (count > inode->size - offset) count = inode->size - offset;
    if (count) memcpy(buffer, inode->data + offset, count);
    irq_spin_unlock(&inode->lock);
    return (long)count;
}

static long ramfs_node_write(struct vfs_node *vnode, size_t offset,
                             const void *buffer, size_t count) {
    struct ramfs_inode *inode = inode_of(vnode);
    if (!inode || count > (size_t)-1 - offset) return -EINVAL;
    if (vnode->type != VFS_NODE_REGULAR) return -EISDIR;
    irq_spin_lock(&inode->lock);
    size_t end = offset + count;
    int result = ensure_capacity_locked(inode, end);
    if (result < 0) {
        irq_spin_unlock(&inode->lock);
        return result;
    }
    if (offset > inode->size)
        memset(inode->data + inode->size, 0, offset - inode->size);
    if (count) memcpy(inode->data + offset, buffer, count);
    if (end > inode->size) inode->size = end;
    irq_spin_unlock(&inode->lock);
    return (long)count;
}

static int ramfs_node_truncate(struct vfs_node *vnode, size_t size) {
    struct ramfs_inode *inode = inode_of(vnode);
    if (!inode) return -EINVAL;
    if (vnode->type != VFS_NODE_REGULAR) return -EISDIR;
    irq_spin_lock(&inode->lock);
    int result = ensure_capacity_locked(inode, size);
    if (result == 0) {
        if (size > inode->size)
            memset(inode->data + inode->size, 0, size - inode->size);
        inode->size = size;
    }
    irq_spin_unlock(&inode->lock);
    return result;
}

static int ramfs_node_readdir(struct vfs_node *vnode, size_t index,
                              struct vfs_dirent *entry) {
    struct ramfs_inode *inode = inode_of(vnode);
    if (!inode || !entry) return -EINVAL;
    if (vnode->type != VFS_NODE_DIRECTORY) return -ENOTDIR;

    irq_spin_lock(&tree_lock);
    struct ramfs_dentry *child = inode->children;
    while (child && index--) child = child->next_sibling;
    if (!child) {
        irq_spin_unlock(&tree_lock);
        return 0;
    }
    struct ramfs_inode *child_inode = inode_of(child->dentry.node);
    entry->ino = child_inode->ino;
    entry->type = child->dentry.node->type;
    memcpy(entry->name, child->dentry.name, strlen(child->dentry.name) + 1);
    irq_spin_unlock(&tree_lock);
    return 1;
}

static int ramfs_node_getattr(struct vfs_node *vnode, struct vfs_attr *attr) {
    struct ramfs_inode *inode = inode_of(vnode);
    if (!inode || !attr) return -EINVAL;
    irq_spin_lock(&inode->lock);
    attr->ino = inode->ino;
    attr->type = vnode->type;
    attr->mode = inode->mode;
    attr->nlink = inode->nlink;
    attr->uid = inode->uid;
    attr->gid = inode->gid;
    attr->size = inode->size;
    irq_spin_unlock(&inode->lock);
    return 0;
}

static long ramfs_node_readlink(struct vfs_node *vnode, void *buffer,
                                size_t size) {
    struct ramfs_inode *inode = inode_of(vnode);
    if (!inode || vnode->type != VFS_NODE_SYMLINK)
        return -EINVAL;
    irq_spin_lock(&inode->lock);
    if (size > inode->size) size = inode->size;
    if (size) memcpy(buffer, inode->data, size);
    irq_spin_unlock(&inode->lock);
    return (long)size;
}

static void ramfs_node_destroy(struct vfs_node *vnode) {
    struct ramfs_inode *inode = inode_of(vnode);
    if (!inode || inode == &root_inode) return;
    if (inode->owns_data) kfree(inode->data);
    kfree(inode);
}

static void ramfs_destroy_dentry(struct vfs_dentry *entry) {
    struct ramfs_dentry *dentry = dentry_of(entry);
    if (dentry && dentry != &root_dentry)
        kfree(dentry);
}

static const struct vfs_fs_ops ramfs_fs_ops = {
    .root = ramfs_root,
    .lookup_child = ramfs_lookup_child,
    .create = ramfs_create,
    .remove = ramfs_remove,
    .rename = ramfs_rename,
    .link = ramfs_link,
    .symlink = ramfs_symlink,
    .destroy_dentry = ramfs_destroy_dentry,
};
