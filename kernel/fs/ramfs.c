#include <kernel/fs/ramfs.h>
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

static struct ramfs_node root;
static struct ramfs_node *nodes;
static spinlock_t tree_lock = SPINLOCK_INIT;

static int resolve(const char *cwd, const char *path, char out[101]) {
    size_t length = 0;
    if (*path != '/' && cwd && *cwd) {
        length = strlen(cwd);
        if (length >= 101) return -1;
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
        if (length && length < 100) out[length++] = '/';
        if (part > 100 - length) return -1;
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
    node->lock = (spinlock_t)SPINLOCK_INIT;
    memcpy(node->path, path, length + 1);
    node->directory = directory;
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
    root.directory = true;
    nodes = NULL;
    tar_foreach(seed_tar_file, NULL);
}

int ramfs_open(const char *cwd, const char *raw_path, int flags,
               struct ramfs_node **out) {
    char resolved[101];
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

    if (!node || node->directory != !!(flags & O_DIRECTORY)) {
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
    *out = node;
    return 0;
}

int ramfs_mkdir(const char *cwd, const char *raw_path) {
    char resolved[101];
    if (resolve(cwd, raw_path, resolved) != 0 || !resolved[0]) return -1;
    const char *path = resolved;
    irq_spin_lock(&tree_lock);
    struct ramfs_node *existing = find_locked(path);
    if (existing) {
        int result = existing->directory ? 0 : -1;
        irq_spin_unlock(&tree_lock);
        return result;
    }
    struct ramfs_node *node = new_node_locked(path, true);
    irq_spin_unlock(&tree_lock);
    return node ? 0 : -1;
}

int ramfs_chdir(const char *cwd, const char *path, char *out, size_t out_size) {
    char resolved[101];
    if (!out || resolve(cwd, path, resolved) != 0) return -1;
    irq_spin_lock(&tree_lock);
    struct ramfs_node *node = find_locked(resolved);
    int ok = node && node->directory;
    irq_spin_unlock(&tree_lock);
    size_t length = strlen(resolved) + 1;
    if (!ok || length > out_size) return -1;
    memcpy(out, resolved, length);
    return 0;
}

int ramfs_lookup(const char *cwd, const char *path, struct ramfs_node **out) {
    char resolved[101];
    if (!out || resolve(cwd, path, resolved) != 0) return -1;
    irq_spin_lock(&tree_lock);
    *out = find_locked(resolved);
    irq_spin_unlock(&tree_lock);
    return *out ? 0 : -1;
}

int ramfs_readdir(struct ramfs_node *directory, size_t index,
                  char *name, size_t name_size, unsigned char *type) {
    if (!directory || !directory->directory || !name || !name_size) return -1;
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
        if (length > name_size) { irq_spin_unlock(&tree_lock); return -1; }
        memcpy(name, child, length);
        if (type) *type = node->directory ? 4 : 8;
        irq_spin_unlock(&tree_lock);
        return 1;
    }
    irq_spin_unlock(&tree_lock);
    return 0;
}

long ramfs_read(struct ramfs_node *node, size_t offset, void *buffer, size_t count) {
    if (!node || node->directory) return -1;
    irq_spin_lock(&node->lock);
    if (offset > node->size) offset = node->size;
    if (count > node->size - offset) count = node->size - offset;
    if (count)
        memcpy(buffer, node->data + offset, count);
    irq_spin_unlock(&node->lock);
    return (long)count;
}

long ramfs_write(struct ramfs_node *node, size_t offset,
                 const void *buffer, size_t count) {
    if (!node || node->directory || count > (size_t)-1 - offset) return -1;
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

size_t ramfs_size(struct ramfs_node *node) {
    irq_spin_lock(&node->lock);
    size_t size = node->size;
    irq_spin_unlock(&node->lock);
    return size;
}
