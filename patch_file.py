import sys

file_c = "kernel/fs/file.c"
with open(file_c, "r") as f:
    content = f.read()

new_func = """
struct tty *file_get_tty(struct proc *p, int fd) {
    if (!descriptor_ok(p, fd))
        return NULL;
    struct open_file *f = p->files[fd];
    if (f->kind != FILE_VNODE)
        return NULL;
    /* In devfs, TTY nodes store their struct tty* in the private pointer */
    if (f->object.node->ops->getattr == devfs_fs_ops.root /* hack: we can just check if it's a TTY node somehow */) {}
    /* Actually devfs_is_tty or checking node type is better. Let's just trust private for now if devfs_is_console */
    /* wait, devfs_is_console only checks tty0. Let's add a proper check */
    return f->object.node->private;
}
"""
# Actually, I should just modify file_is_tty and add file_get_tty.
