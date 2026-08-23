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

static const char *normalize(const char *path) {
    while (*path == '/') path++;
    while (path[0] == '.' && path[1] == '/') path += 2;
    return path;
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

void ramfs_init(void) {
    memset(&root, 0, sizeof(root));
    root.lock = (spinlock_t)SPINLOCK_INIT;
    root.directory = true;
    nodes = NULL;
}

int ramfs_open(const char *raw_path, int flags, struct ramfs_node **out) {
    const char *path = normalize(raw_path);
    int access = flags & O_ACCMODE;
    irq_spin_lock(&tree_lock);
    struct ramfs_node *node = find_locked(path);

    if (!node) {
        struct tar_file tar;
        if (tar_open(path, &tar)) {
            node = new_node_locked(path, false);
            if (node) {
                node->data = tar.data;
                node->size = tar.size;
                node->capacity = tar.size;
                node->owns_data = false;
            }
        } else if (flags & O_CREAT) {
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

int ramfs_mkdir(const char *raw_path) {
    const char *path = normalize(raw_path);
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
