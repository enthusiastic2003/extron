#include <kernel/fs/ramfs.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/tar.h>
#include <kernel/mm/kheap.h>
#include <kernel/klibc/string.h>
#include <arch/irq_spinlock.h>

#define O_ACCMODE 03
#define O_WRONLY  01
#define O_RDWR    02
#define O_CREAT   0100
#define O_EXCL    0200
#define O_TRUNC   01000
#define O_DIRECTORY 0200000

struct ramfs_node {
    struct vfs_node vnode;
    spinlock_t lock;
    char path[VFS_PATH_MAX + 1];
    bool owns_data;
    uint8_t *data;
    size_t size;
    size_t capacity;
    uint64_t ino;
    uint32_t mode;
    struct ramfs_node *next;
};

static struct ramfs_node root;
static struct ramfs_node *nodes;
static spinlock_t tree_lock = SPINLOCK_INIT;
static uint64_t next_ino;
static const struct vfs_fs_ops ramfs_fs_ops;

static long ramfs_node_read(struct vfs_node *, size_t, void *, size_t);
static long ramfs_node_write(struct vfs_node *, size_t, const void *, size_t);
static int ramfs_node_readdir(struct vfs_node *, size_t, struct vfs_dirent *);
static int ramfs_node_getattr(struct vfs_node *, struct vfs_attr *);

static const struct vfs_node_ops ramfs_node_ops = {
    .read = ramfs_node_read,
    .write = ramfs_node_write,
    .readdir = ramfs_node_readdir,
    .getattr = ramfs_node_getattr,
};

static int resolve(const char *cwd, const char *path,
                   char out[VFS_PATH_MAX + 1]) {
    size_t length = 0;
    if (*path != '/' && cwd && *cwd) {
        length = strlen(cwd);
        if (length > VFS_PATH_MAX) return -1;
        memcpy(out, cwd, length);
    }
    while (*path) {
        while (*path == '/') path++;
        const char *start = path;
        while (*path && *path != '/') path++;
        size_t part = (size_t)(path - start);
        if (!part || (part == 1 && start[0] == '.')) continue;
        if (part == 2 && start[0] == '.' && start[1] == '.') {
            while (length && out[length - 1] != '/') length--;
            if (length) length--;
            continue;
        }
        if (length && length < VFS_PATH_MAX) out[length++] = '/';
        if (part > VFS_PATH_MAX - length) return -1;
        memcpy(out + length, start, part);
        length += part;
    }
    out[length] = '\0';
    return 0;
}

static struct ramfs_node *find_locked(const char *path) {
    if (!*path || (path[0] == '.' && path[1] == '\0'))
        return &root;
    for (struct ramfs_node *n = nodes; n; n = n->next)
        if (strcmp(n->path, path) == 0)
            return n;
    return NULL;
}

static struct ramfs_node *new_node_locked(const char *path, bool directory) {
    size_t length = strlen(path);
    if (!length || length >= sizeof(((struct ramfs_node *)0)->path))
        return NULL;
    struct ramfs_node *node = kmalloc(sizeof(*node));
    if (!node) return NULL;
    memset(node, 0, sizeof(*node));
    node->vnode.ops = &ramfs_node_ops;
    node->vnode.private = node;
    node->vnode.type = directory ? VFS_NODE_DIRECTORY : VFS_NODE_REGULAR;
    node->lock = (spinlock_t)SPINLOCK_INIT;
    memcpy(node->path, path, length + 1);
    node->ino = next_ino++;
    node->mode = directory ? 0755 : 0644;
    node->next = nodes;
    nodes = node;
    return node;
}

static void seed_tar_file(const struct tar_file *file, void *context) {
    (void)context;
    struct ramfs_node *node = new_node_locked(file->name, false);
    if (!node) return;
    node->data = file->data;
    node->size = node->capacity = file->size;
}

void ramfs_init(void) {
    memset(&root, 0, sizeof(root));
    root.lock = (spinlock_t)SPINLOCK_INIT;
    root.vnode.ops = &ramfs_node_ops;
    root.vnode.private = &root;
    root.vnode.type = VFS_NODE_DIRECTORY;
    root.ino = 1;
    root.mode = 0755;
    next_ino = 2;
    nodes = NULL;
    tar_foreach(seed_tar_file, NULL);
    vfs_mount_root(&ramfs_fs_ops, NULL);
}

static int ramfs_open(struct vfs_mount *mount, const char *cwd,
                      const char *raw_path, int flags,
                      struct vfs_node **out) {
    (void)mount;
    char resolved[VFS_PATH_MAX + 1];
    if (resolve(cwd, raw_path, resolved) != 0) return -1;
    const char *path = resolved;
    int access = flags & O_ACCMODE;
    irq_spin_lock(&tree_lock);
    struct ramfs_node *node = find_locked(path);

    if (!node) {
        if (flags & O_CREAT) {
            node = new_node_locked(path, false);
        }
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        irq_spin_unlock(&tree_lock);
        return -1;
    }

    bool directory = node && node->vnode.type == VFS_NODE_DIRECTORY;
    if (!node || directory != !!(flags & O_DIRECTORY)) {
        irq_spin_unlock(&tree_lock);
        return -1;
    }
    irq_spin_unlock(&tree_lock);

    if ((flags & O_TRUNC) && access != 0) {
        irq_spin_lock(&node->lock);
        if (node->owns_data) kfree(node->data);
        node->data = NULL;
        node->size = node->capacity = 0;
        node->owns_data = true;
        irq_spin_unlock(&node->lock);
    }
    *out = &node->vnode;
    return 0;
}

static int ramfs_mkdir(struct vfs_mount *mount, const char *cwd,
                       const char *raw_path, uint32_t mode) {
    (void)mount;
    char resolved[VFS_PATH_MAX + 1];
    if (resolve(cwd, raw_path, resolved) != 0 || !resolved[0]) return -1;
    const char *path = resolved;
    irq_spin_lock(&tree_lock);
    struct ramfs_node *existing = find_locked(path);
    if (existing) {
        int result = existing->vnode.type == VFS_NODE_DIRECTORY ? 0 : -1;
        irq_spin_unlock(&tree_lock);
        return result;
    }
    struct ramfs_node *node = new_node_locked(path, true);
    if (node)
        node->mode = mode & 0777;
    irq_spin_unlock(&tree_lock);
    return node ? 0 : -1;
}

static int ramfs_resolve_directory(struct vfs_mount *mount, const char *cwd,
                                   const char *path, char *out,
                                   size_t out_size) {
    (void)mount;
    char resolved[VFS_PATH_MAX + 1];
    if (!out || resolve(cwd, path, resolved) != 0) return -1;
    irq_spin_lock(&tree_lock);
    struct ramfs_node *node = find_locked(resolved);
    int ok = node && node->vnode.type == VFS_NODE_DIRECTORY;
    irq_spin_unlock(&tree_lock);
    size_t length = strlen(resolved) + 1;
    if (!ok || length > out_size) return -1;
    memcpy(out, resolved, length);
    return 0;
}

static int ramfs_lookup(struct vfs_mount *mount, const char *cwd,
                        const char *path, struct vfs_node **out) {
    (void)mount;
    char resolved[VFS_PATH_MAX + 1];
    if (!out || resolve(cwd, path, resolved) != 0) return -1;
    irq_spin_lock(&tree_lock);
    struct ramfs_node *node = find_locked(resolved);
    *out = node ? &node->vnode : NULL;
    irq_spin_unlock(&tree_lock);
    return *out ? 0 : -1;
}

static int ramfs_node_readdir(struct vfs_node *vnode, size_t index,
                              struct vfs_dirent *entry) {
    struct ramfs_node *directory = vnode ? vnode->private : NULL;
    if (!directory || vnode->type != VFS_NODE_DIRECTORY || !entry) return -1;
    const char *base = directory == &root ? "" : directory->path;
    size_t base_length = strlen(base);
    irq_spin_lock(&tree_lock);
    size_t current = 0;
    for (struct ramfs_node *node = nodes; node; node = node->next) {
        const char *child = node->path;
        if (base_length) {
            if (strncmp(child, base, base_length) != 0 || child[base_length] != '/')
                continue;
            child += base_length + 1;
        }
        if (!*child || strchr(child, '/')) continue;
        if (current++ != index) continue;
        size_t length = strlen(child) + 1;
        if (length > sizeof(entry->name)) { irq_spin_unlock(&tree_lock); return -1; }
        memcpy(entry->name, child, length);
        entry->ino = node->ino;
        entry->type = node->vnode.type;
        irq_spin_unlock(&tree_lock);
        return 1;
    }
    irq_spin_unlock(&tree_lock);
    return 0;
}

static long ramfs_node_read(struct vfs_node *vnode, size_t offset,
                            void *buffer, size_t count) {
    struct ramfs_node *node = vnode ? vnode->private : NULL;
    if (!node || vnode->type != VFS_NODE_REGULAR) return -1;
    irq_spin_lock(&node->lock);
    if (offset > node->size) offset = node->size;
    if (count > node->size - offset) count = node->size - offset;
    if (count)
        memcpy(buffer, node->data + offset, count);
    irq_spin_unlock(&node->lock);
    return (long)count;
}

static long ramfs_node_write(struct vfs_node *vnode, size_t offset,
                             const void *buffer, size_t count) {
    struct ramfs_node *node = vnode ? vnode->private : NULL;
    if (!node || vnode->type != VFS_NODE_REGULAR
            || count > (size_t)-1 - offset) return -1;
    irq_spin_lock(&node->lock);
    size_t end = offset + count;
    if (!node->owns_data || end > node->capacity) {
        size_t capacity = node->capacity ? node->capacity : 256;
        while (capacity < end) {
            if (capacity > (size_t)-1 / 2) { capacity = end; break; }
            capacity *= 2;
        }
        uint8_t *data = kmalloc(capacity);
        if (!data) { irq_spin_unlock(&node->lock); return -1; }
        memset(data, 0, capacity);
        if (node->size) memcpy(data, node->data, node->size);
        if (node->owns_data) kfree(node->data);
        node->data = data;
        node->capacity = capacity;
        node->owns_data = true;
    }
    if (offset > node->size) memset(node->data + node->size, 0, offset - node->size);
    if (count) memcpy(node->data + offset, buffer, count);
    if (end > node->size) node->size = end;
    irq_spin_unlock(&node->lock);
    return (long)count;
}

static int ramfs_node_getattr(struct vfs_node *vnode, struct vfs_attr *attr) {
    struct ramfs_node *node = vnode ? vnode->private : NULL;
    if (!node || !attr)
        return -1;
    irq_spin_lock(&node->lock);
    attr->ino = node->ino;
    attr->type = vnode->type;
    attr->mode = node->mode;
    attr->nlink = 1;
    attr->uid = 0;
    attr->gid = 0;
    attr->size = node->size;
    irq_spin_unlock(&node->lock);
    return 0;
}

static const struct vfs_fs_ops ramfs_fs_ops = {
    .open = ramfs_open,
    .lookup = ramfs_lookup,
    .mkdir = ramfs_mkdir,
    .resolve_directory = ramfs_resolve_directory,
};
