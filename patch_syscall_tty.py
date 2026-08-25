import sys

syscall_c = "kernel/proc/syscall.c"
with open(syscall_c, "r") as f:
    content = f.read()

# Replace file_is_tty with getting the tty and using it
# For sys_ioctl:
content = content.replace("    if (!file_is_tty(p, (int)fd))\n        return (uint64_t)-ENOTTY;", "    struct tty *t = file_get_tty(p, (int)fd);\n    if (!t)\n        return (uint64_t)-ENOTTY;")
content = content.replace("tty_get_termios((struct tty_termios *)arg);", "tty_get_termios(t, (struct tty_termios *)arg);")
content = content.replace("tty_set_termios((const struct tty_termios *)arg, request == TCSETSF);", "tty_set_termios(t, (const struct tty_termios *)arg, request == TCSETSF);")
content = content.replace("tty_get_winsize((struct tty_winsize *)arg);", "tty_get_winsize(t, (struct tty_winsize *)arg);")
content = content.replace("tty_set_winsize((const struct tty_winsize *)arg);", "tty_set_winsize(t, (const struct tty_winsize *)arg);")
content = content.replace("*(int *)arg = (int)tty_foreground_pgid();", "*(int *)arg = (int)tty_foreground_pgid(t);")
content = content.replace("tty_set_foreground_pgid((uint64_t)pgid);", "tty_set_foreground_pgid(t, (uint64_t)pgid);")

with open(syscall_c, "w") as f:
    f.write(content)
