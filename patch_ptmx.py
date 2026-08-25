import sys

devfs_h = "kernel/include/kernel/fs/devfs.h"
with open(devfs_h, "a") as f:
    f.write("\nbool devfs_is_ptmx(struct vfs_node *node);\n")

devfs_c = "kernel/fs/devfs.c"
with open(devfs_c, "a") as f:
    f.write("\nbool devfs_is_ptmx(struct vfs_node *node) {\n    return node == &ptmx_node;\n}\n")

