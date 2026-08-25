import sys

devfs_c = "kernel/fs/devfs.c"
with open(devfs_c, "r") as f:
    content = f.read()

# Includes
content = "#include <kernel/drivers/pty.h>\n#include <kernel/mm/kheap.h>\n" + content

# Nodes
content = content.replace("static struct vfs_node null_node;", "static struct vfs_node ptmx_node;\nstatic struct vfs_node null_node;")

# Entries
content = content.replace('{ "tty",     &console_node, {0} },', '{ "ptmx",    &ptmx_node,    {0} },\n    { "tty",     &console_node, {0} },')

# Init
content = content.replace("vfs_node_init(&null_node, &null_ops, NULL, VFS_NODE_DEVICE);", "vfs_node_init(&ptmx_node, &null_ops, NULL, VFS_NODE_DEVICE);\n    vfs_node_init(&null_node, &null_ops, NULL, VFS_NODE_DEVICE);")

# devfs_get_tty
content = content.replace("bool devfs_is_console(struct vfs_node *node) {\n    return node == &console_node;\n}", """struct tty *devfs_get_tty(struct vfs_node *node) {
    if (node == &console_node) return &tty_table[0];
    if (node->ops == &console_ops && node != &console_node) {
        return (struct tty *)node->private;
    }
    return NULL;
}
bool devfs_is_ptmx(struct vfs_node *node) {
    return node == &ptmx_node;
}""")

# devfs_lookup_child
idx = content.find("for (size_t i = 0; i < ENTRY_COUNT; i++)")
new_lookup = """
    if (name[0] == 'p' && name[1] == 't' && name[2] == 's') {
        int index = 0;
        int i = 3;
        while (name[i] >= '0' && name[i] <= '9') {
            index = index * 10 + (name[i] - '0');
            i++;
        }
        if (name[i] == '\\0') {
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
    """
content = content[:idx] + new_lookup + content[idx:]

with open(devfs_c, "w") as f:
    f.write(content)

