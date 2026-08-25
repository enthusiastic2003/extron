import sys

file_c = "kernel/fs/file.c"
with open(file_c, "r") as f:
    content = f.read()

start = content.find("if (f->kind == FILE_VNODE && devfs_is_console(f->object.node)) {")
end = content.find("} else if (f->kind == FILE_VNODE) {")

new_poll = """struct tty *t = NULL;
    if (f->kind == FILE_VNODE)
        t = devfs_get_tty(f->object.node);
    if (t) {
        if ((events & POLLIN) && tty_input_ready(t))
            *revents |= POLLIN;
        if (events & POLLOUT)
            *revents |= POLLOUT;
    """
content = content[:start] + new_poll + content[end:]
with open(file_c, "w") as f:
    f.write(content)
