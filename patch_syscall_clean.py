import sys

syscall_c = "kernel/proc/syscall.c"
with open(syscall_c, "r") as f:
    content = f.read()

# Includes
content = "#include <kernel/drivers/pty.h>\n" + content

# TTY ioctls
idx = content.find("uint64_t sys_ioctl(")
idx = content.find("if (!file_is_tty(p, (int)fd))", idx)

new_code = """
    struct open_file *f_ioctl = p->files[fd];
    if (f_ioctl && f_ioctl->kind == FILE_VNODE && pty_get_index(f_ioctl->object.node) >= 0) {
        if (request == 0x80045430) { /* TIOCGPTN */
            if (!user_buffer_ok(p, arg, sizeof(int))) return (uint64_t)-EFAULT;
            *(int *)arg = pty_get_index(f_ioctl->object.node);
            return 0;
        }
        if (request == 0x40045431) { /* TIOCSPTLCK */
            if (!user_buffer_ok(p, arg, sizeof(int))) return (uint64_t)-EFAULT;
            return 0; // successfully fake unlocked
        }
    }
    
    struct tty *t = file_get_tty(p, (int)fd);
    if (!t)
"""
content = content[:idx] + new_code.strip() + content[idx + len("if (!file_is_tty(p, (int)fd))"):]
content = content.replace("tty_get_termios((struct tty_termios *)arg);", "tty_get_termios(t, (struct tty_termios *)arg);")
content = content.replace("tty_set_termios((const struct tty_termios *)arg, request == TCSETSF);", "tty_set_termios(t, (const struct tty_termios *)arg, request == TCSETSF);")
content = content.replace("tty_get_winsize((struct tty_winsize *)arg);", "tty_get_winsize(t, (struct tty_winsize *)arg);")
content = content.replace("tty_set_winsize((const struct tty_winsize *)arg);", "tty_set_winsize(t, (const struct tty_winsize *)arg);")
content = content.replace("*(int *)arg = (int)tty_foreground_pgid();", "*(int *)arg = (int)tty_foreground_pgid(t);")
content = content.replace("tty_set_foreground_pgid((uint64_t)pgid);", "tty_set_foreground_pgid(t, (uint64_t)pgid);")


# Poll
start = content.find("bool waits_for_input = false;")
end = content.find("if (signal_pending_unblocked(my_thread()))", start)
new_poll = """struct tty *wait_tty = NULL;
    for (size_t i = 0; i < count; i++) {
        struct tty *t = file_get_tty(my_proc(), fds[i].fd);
        if (t && (fds[i].events & POLLIN)) {
            wait_tty = t;
            break;
        }
    }
    if (!wait_tty)
        return 0;
    tty_wait_for_input(wait_tty, timeout);
    """
content = content[:start] + new_poll + content[end:]

with open(syscall_c, "w") as f:
    f.write(content)
