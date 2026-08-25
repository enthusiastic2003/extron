import sys

file_c = "kernel/fs/file.c"
with open(file_c, "r") as f:
    content = f.read()

start_idx = content.find("int file_is_tty(struct proc *p, int fd) {")

new_code = """
int file_is_tty(struct proc *p, int fd) {
    if (!descriptor_ok(p, fd))
        return 0;
    struct open_file *f = p->files[fd];
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

int file_poll(struct proc *p, int fd, short events, short *revents) {
    if (!revents)
        return -EINVAL;
    *revents = 0;
    if (!descriptor_ok(p, fd))
        return -EBADF;
    struct open_file *f = p->files[fd];
    
    struct tty *t = NULL;
    if (f->kind == FILE_VNODE)
        t = devfs_get_tty(f->object.node);
        
    if (t) {
        if ((events & POLLIN) && tty_input_ready(t))
            *revents |= POLLIN;
        if (events & POLLOUT)
            *revents |= POLLOUT;
    } else if (f->kind == FILE_VNODE) {
        if (events & POLLOUT)
            *revents |= POLLOUT;
        if (events & POLLIN)
            *revents |= POLLIN;
    } else {
        struct pipe_buffer *pipe = f->object.pipe;
        irq_spin_lock(&pipe->lock);
        if (f->kind == FILE_PIPE_READER) {
            if ((events & POLLIN) && (pipe->used || !pipe->writers))
                *revents |= POLLIN;
            if (!pipe->writers)
                *revents |= POLLHUP;
        } else {
            if ((events & POLLOUT) && (pipe->used < PIPE_BUFFER_SIZE || !pipe->readers))
                *revents |= POLLOUT;
            if (!pipe->readers)
                *revents |= POLLERR;
        }
        irq_spin_unlock(&pipe->lock);
    }
    return 0;
}
"""
content = content[:start_idx] + new_code.strip() + "\n"
with open(file_c, "w") as f:
    f.write(content)
