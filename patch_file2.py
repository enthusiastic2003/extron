import sys

file_c = "kernel/fs/file.c"
with open(file_c, "r") as f:
    content = f.read()

start_idx = content.find("int file_is_tty")
end_idx = content.find("int file_poll")

new_code = """
int file_is_tty(struct proc *p, int fd) {
    if (!descriptor_ok(p, fd))
        return 0;
    struct open_file *f = p->files[fd];
    // DevFS sets the private pointer to the struct tty for all TTY/PTY nodes
    // Wait, devfs_is_console is still around. But we want devfs_is_tty.
    // Let's implement devfs_get_tty(node) which returns the tty if it's a tty node.
    return f->kind == FILE_VNODE && devfs_get_tty(f->object.node) != NULL;
}

struct tty *file_get_tty(struct proc *p, int fd) {
    if (!descriptor_ok(p, fd))
        return NULL;
    struct open_file *f = p->files[fd];
    if (f->kind == FILE_VNODE)
        return devfs_get_tty(f->object.node);
    return NULL;
}

"""
content = content[:start_idx] + new_code + content[end_idx:]
with open(file_c, "w") as f:
    f.write(content)

file_h = "kernel/include/kernel/fs/file.h"
with open(file_h, "a") as f:
    f.write("struct tty *file_get_tty(struct proc *p, int fd);\n")

