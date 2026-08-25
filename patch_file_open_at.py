import sys

file_c = "kernel/fs/file.c"
with open(file_c, "r") as f:
    content = f.read()

idx = content.find("f->object.node = node;")
insert = """
    if (devfs_is_ptmx(f->object.node)) {
        struct vfs_node *master = pty_allocate_master();
        if (!master) {
            vfs_path_release(&f->path);
            vfs_node_release(node);
            kfree(f);
            return -ENOMEM;
        }
        vfs_node_release(f->object.node);
        f->object.node = master;
        node = master; // for vfs_getattr below
    }
"""
content = content[:idx + len("f->object.node = node;")] + insert + content[idx + len("f->object.node = node;"):]

with open(file_c, "w") as f:
    f.write(content)

