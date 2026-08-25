import sys

devfs_c = "kernel/fs/devfs.c"
with open(devfs_c, "r") as f:
    content = f.read()

# Add ptmx_node
idx = content.find("static struct vfs_node null_node;")
content = content[:idx] + "static struct vfs_node ptmx_node;\n" + content[idx:]

# Add ptmx to entries array
idx = content.find('{ "tty",     &console_node, {0} },')
content = content[:idx] + '{ "ptmx",    &ptmx_node,    {0} },\n    ' + content[idx:]

# Add devfs_get_tty slave logic
idx = content.find("/* Support for PTY slaves will be added here */")
new_slave = """if (node->ops == &console_ops && node != &console_node) {
        return (struct tty *)node->private;
    }"""
content = content[:idx] + new_slave + content[idx + len("/* Support for PTY slaves will be added here */"):]

# Update devfs_lookup_child for pts/N
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
                // Dynamically allocate a dentry and node for this lookup.
                // In a real OS this would be in a devpts filesystem.
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

# Update devfs_init
idx = content.find("vfs_node_init(&null_node, &null_ops, NULL, VFS_NODE_DEVICE);")
content = content[:idx] + "vfs_node_init(&ptmx_node, &null_ops, NULL, VFS_NODE_DEVICE); /* ops unused, intercepted in sys_open */\n    " + content[idx:]

with open(devfs_c, "w") as f:
    f.write(content)
