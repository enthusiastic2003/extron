#include <kernel/console.h>
#include <kernel/drivers/pty.h>
#include <kernel/mm/kheap.h>
#include <kernel/fs/devfs.h>
#include <kernel/drivers/tty.h>
#include <kernel/drivers/fb.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/mm/pmm.h>
#include <kernel/klibc/string.h>
#include <kernel/errno.h>

static struct vfs_node console_node;
static struct vfs_node ptmx_node;
static struct vfs_node null_node;
static struct vfs_node zero_node;
static struct vfs_node fb_node;
static struct vfs_node input_node;
static struct vfs_node root_node;
static struct vfs_dentry root_dentry;

struct devfs_entry {
    const char *name;
    struct vfs_node *node;
    struct vfs_dentry dentry;
};

static struct devfs_entry entries[] = {
    { .name = "ptmx",    .node = &ptmx_node },
    { .name = "console", .node = &console_node },
    { .name = "tty",     .node = &console_node },
    { .name = "null",    .node = &null_node },
    { .name = "zero",    .node = &zero_node },
    { .name = "fb0",     .node = &fb_node },
    { .name = "input",   .node = &input_node },
};
#define ENTRY_COUNT (sizeof(entries) / sizeof(entries[0]))

static long console_read(struct vfs_node *n, size_t off, void *buf, size_t count) {
    (void)n; (void)off;
    return tty_read(devfs_get_tty(n), buf, count);
}

static long console_write(struct vfs_node *n, size_t off, const void *buf, size_t count) {
    (void)n; (void)off;
    return tty_write(devfs_get_tty(n), buf, count);
}

static long null_read(struct vfs_node *n, size_t off, void *buf, size_t count) {
    (void)n; (void)off; (void)buf; (void)count;
    return 0;
}

static long null_write(struct vfs_node *n, size_t off, const void *buf, size_t count) {
    (void)n; (void)off; (void)buf;
    return (long)count;
}

static long zero_read(struct vfs_node *n, size_t off, void *buf, size_t count) {
    (void)n; (void)off;
    memset(buf, 0, count);
    return (long)count;
}

/* Geometry as a flat struct, read in one call — usr/include/extron/fb.h's
 * struct extron_fb_geometry must match this byte for byte. A real ioctl
 * (FBIOGET_VSCREENINFO-style) would need sys_ioctl() to stop assuming
 * every fd is the console (kernel/proc/syscall.c's sys_ioctl() gates on
 * file_is_tty() first); read() needs no such change and this device has
 * no other use for read() anyway. */
struct fb_geometry {
    uint32_t width, height, pitch, depth, rgb_order, size;
};

static long fb_read(struct vfs_node *n, size_t off, void *buf, size_t count) {
    (void)n;
    const struct framebuffer *fb = fb_get();
    if (!fb) return -ENODEV;
    struct fb_geometry geo = {
        .width = fb->width, .height = fb->height, .pitch = fb->pitch,
        .depth = fb->depth, .rgb_order = fb->rgb_order, .size = fb->size,
    };
    if (off >= sizeof(geo)) return 0;
    size_t avail = sizeof(geo) - off;
    if (count > avail) count = avail;
    memcpy(buf, (uint8_t *)&geo + off, count);
    return (long)count;
}

static int fb_mmap(struct vfs_node *n, size_t offset, size_t length,
                   uint64_t *out_phys, size_t *out_size, bool *out_cacheable) {
    (void)n;
    const struct framebuffer *fb = fb_get();
    if (!fb) return -ENODEV;
    if (offset > fb->size || length > fb->size - offset)
        return -EINVAL;
    *out_phys = (uint64_t)fb->phys + offset;
    *out_size = length;
    /* The GPU scans this memory out continuously — see VM_NOCACHE's own
     * comment (kernel/include/kernel/mm/paging.h). */
    *out_cacheable = false;
    return 0;
}

/* Backed by the same page the kernel's UART ISR pushes bytes into
 * (kernel/drivers/keyboard.c) — real RAM, not MMIO, so unlike the
 * framebuffer this stays ordinarily cacheable: the kernel and the
 * process share it the same way any two cache-coherent contexts on
 * this machine would, with dmb ish barriers (already in
 * extron_input_getc(), usr/include/extron/fb.h) doing the ordering
 * work instead of PAGE_NORMAL_NC. */
static int input_mmap(struct vfs_node *n, size_t offset, size_t length,
                      uint64_t *out_phys, size_t *out_size, bool *out_cacheable) {
    (void)n;
    phys_addr_t ring = kbd_ring_phys();
    if (!ring) return -ENODEV;
    if (offset > PAGE_SIZE || length > PAGE_SIZE - offset)
        return -EINVAL;
    *out_phys = (uint64_t)ring + offset;
    *out_size = length;
    *out_cacheable = true;
    return 0;
}

static int device_getattr(struct vfs_node *n, struct vfs_attr *attr) {
    memset(attr, 0, sizeof(*attr));
    attr->ino = n == &console_node ? 2 : n == &null_node ? 3
              : n == &zero_node ? 4 : n == &fb_node ? 5 : 6;
    attr->type = VFS_NODE_DEVICE;
    attr->mode = 0666;
    attr->nlink = (n == &console_node) ? 2 : 1;
    return 0;
}

static const struct vfs_node_ops console_ops = {
    .read = console_read,
    .write = console_write,
    .getattr = device_getattr,
};

static const struct vfs_node_ops null_ops = {
    .read = null_read,
    .write = null_write,
    .getattr = device_getattr,
};

static const struct vfs_node_ops zero_ops = {
    .read = zero_read,
    .write = null_write,
    .getattr = device_getattr,
};

static const struct vfs_node_ops fb_ops = {
    .read = fb_read,
    .mmap = fb_mmap,
    .getattr = device_getattr,
};

static const struct vfs_node_ops input_ops = {
    .mmap = input_mmap,
    .getattr = device_getattr,
};

static long root_read(struct vfs_node *n, size_t off, void *buf, size_t count) {
    (void)n; (void)off; (void)buf; (void)count;
    return -EISDIR;
}

static int root_getattr(struct vfs_node *n, struct vfs_attr *attr) {
    (void)n;
    memset(attr, 0, sizeof(*attr));
    attr->ino = 1;
    attr->type = VFS_NODE_DIRECTORY;
    attr->mode = 0755;
    attr->nlink = 2;
    return 0;
}

static int root_readdir(struct vfs_node *n, size_t index, struct vfs_dirent *entry) {
    (void)n;
    if (index >= ENTRY_COUNT)
        return 0;
    struct vfs_attr attr;
    entries[index].node->ops->getattr(entries[index].node, &attr);
    entry->ino = attr.ino;
    entry->type = entries[index].node->type;
    memcpy(entry->name, entries[index].name, strlen(entries[index].name) + 1);
    return 1;
}

static const struct vfs_node_ops root_ops = {
    .read = root_read,
    .readdir = root_readdir,
    .getattr = root_getattr,
};

static int devfs_root(struct vfs_mount *mount, struct vfs_dentry **out) {
    (void)mount;
    vfs_dentry_retain(&root_dentry);
    *out = &root_dentry;
    return 0;
}

static int devfs_lookup_child(struct vfs_mount *mount, struct vfs_dentry *parent,
                              const char *name, struct vfs_dentry **out) {
    (void)mount; (void)parent;
    
    if (name[0] == 'p' && name[1] == 't' && name[2] == 's') {
        int index = 0;
        int i = 3;
        while (name[i] >= '0' && name[i] <= '9') {
            index = index * 10 + (name[i] - '0');
            i++;
        }
        if (name[i] == '\0') {
            struct tty *slave = pty_get_slave(index);
            if (slave) {
                struct vfs_node *pts_node = kmalloc(sizeof(struct vfs_node));
                vfs_node_init(pts_node, &console_ops, slave, VFS_NODE_DEVICE);
                struct vfs_dentry *pts_dentry = kmalloc(sizeof(struct vfs_dentry));
                vfs_dentry_init(pts_dentry, pts_node, parent, name, NULL);
                *out = pts_dentry;
                return 0;
            }
        }
    }
    for (size_t i = 0; i < ENTRY_COUNT; i++)
        if (strcmp(entries[i].name, name) == 0) {
            vfs_dentry_retain(&entries[i].dentry);
            *out = &entries[i].dentry; 
            return 0;
        }
     return -ENOENT;
}

/* /dev is a fixed namespace: no file is ever created, removed, renamed, or
 * linked into it at runtime. These exist only so vfs_open()/vfs_mkdir()/etc,
 * which call through these pointers unconditionally, never dereference a
 * null one if userspace tries. */
static int devfs_create(struct vfs_mount *m, struct vfs_dentry *p, const char *n,
                        enum vfs_node_type t, uint32_t mo, uint32_t u, uint32_t g,
                        struct vfs_dentry **out) {
    (void)m; (void)p; (void)n; (void)t; (void)mo; (void)u; (void)g; (void)out;
    return -EROFS;
}

static int devfs_remove(struct vfs_mount *m, struct vfs_dentry *p, const char *n, int d) {
    (void)m; (void)p; (void)n; (void)d;
    return -EROFS;
}

static int devfs_rename(struct vfs_mount *m, struct vfs_dentry *op, const char *on,
                        struct vfs_dentry *np, const char *nn) {
    (void)m; (void)op; (void)on; (void)np; (void)nn;
    return -EROFS;
}

static int devfs_link(struct vfs_mount *m, struct vfs_node *n, struct vfs_dentry *p,
                      const char *name, struct vfs_dentry **out) {
    (void)m; (void)n; (void)p; (void)name; (void)out;
    return -EROFS;
}

static int devfs_symlink(struct vfs_mount *m, struct vfs_dentry *p, const char *name,
                         const char *target, uint32_t u, uint32_t g,
                         struct vfs_dentry **out) {
    (void)m; (void)p; (void)name; (void)target; (void)u; (void)g; (void)out;
    return -EROFS;
}

static const struct vfs_fs_ops devfs_fs_ops = {
    .root = devfs_root,
    .lookup_child = devfs_lookup_child,
    .create = devfs_create,
    .remove = devfs_remove,
    .rename = devfs_rename,
    .link = devfs_link,
    .symlink = devfs_symlink,
};

struct tty *devfs_get_tty(struct vfs_node *node) {
    if (node == &console_node) return &tty_table[0];
    if (node->ops == &console_ops && node != &console_node) {
        return (struct tty *)node->private;
    }
    return NULL;
}
bool devfs_is_ptmx(struct vfs_node *node) {
    return node == &ptmx_node;
}

void devfs_init(void) {
    vfs_node_init(&console_node, &console_ops, NULL, VFS_NODE_DEVICE);
    vfs_node_init(&ptmx_node, &null_ops, NULL, VFS_NODE_DEVICE);
    vfs_node_init(&null_node, &null_ops, NULL, VFS_NODE_DEVICE);
    vfs_node_init(&zero_node, &zero_ops, NULL, VFS_NODE_DEVICE);
    vfs_node_init(&fb_node, &fb_ops, NULL, VFS_NODE_DEVICE);
    vfs_node_init(&input_node, &input_ops, NULL, VFS_NODE_DEVICE);
    vfs_node_init(&root_node, &root_ops, NULL, VFS_NODE_DIRECTORY);
    vfs_dentry_init(&root_dentry, &root_node, NULL, "", NULL);
    for (size_t i = 0; i < ENTRY_COUNT; i++)
        vfs_dentry_init(&entries[i].dentry, entries[i].node, &root_dentry,
                        entries[i].name, NULL);

    struct vfs_path root;
    if (vfs_root_path(&root) < 0)
        return;
    int res = vfs_mount_at(&root, "/dev", &devfs_fs_ops, NULL); if(res != 0) kprintf("DEVFS MOUNT FAILED %d\n", res);
    vfs_path_release(&root);
}
