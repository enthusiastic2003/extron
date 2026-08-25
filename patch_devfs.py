import sys

devfs_h = "kernel/include/kernel/fs/devfs.h"
with open(devfs_h, "r") as f:
    content = f.read()

content = content.replace("bool devfs_is_console(struct vfs_node *node);", "struct tty *devfs_get_tty(struct vfs_node *node);")
with open(devfs_h, "w") as f:
    f.write(content)

devfs_c = "kernel/fs/devfs.c"
with open(devfs_c, "r") as f:
    content = f.read()

content = content.replace("bool devfs_is_console(struct vfs_node *node) {", "struct tty *devfs_get_tty(struct vfs_node *node) {")
content = content.replace("    return node == &console_node;\n}", "    if (node == &console_node) return &tty_table[0];\n    /* Support for PTY slaves will be added here */\n    if (node->private) return (struct tty *)node->private;\n    return NULL;\n}")
with open(devfs_c, "w") as f:
    f.write(content)
