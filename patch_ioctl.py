import sys

syscall_c = "kernel/proc/syscall.c"
with open(syscall_c, "r") as f:
    content = f.read()

idx = content.find("uint64_t sys_ioctl(")
idx = content.find("if (!t)", idx)
idx_end = content.find("return (uint64_t)-ENOTTY;", idx)

# We want to check for TIOCGPTN on the master fd BEFORE the !t check,
# because master fd is not a TTY.
# Actually, file_get_tty returns NULL for the master fd.
# So we can just put it before `if (!t)`.

new_code = """
    struct open_file *f_ioctl = p->files[fd];
    if (f_ioctl->kind == FILE_VNODE && pty_get_index(f_ioctl->object.node) >= 0) {
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
"""

content = content[:idx] + new_code + content[idx:]
with open(syscall_c, "w") as f:
    f.write(content)
