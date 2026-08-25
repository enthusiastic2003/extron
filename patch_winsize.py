import sys

# 1. Update tty.h
tty_h = "kernel/include/kernel/drivers/tty.h"
with open(tty_h, "r") as f:
    content = f.read()
if "tty_set_winsize" not in content:
    content = content.replace("void tty_get_winsize(struct tty_winsize *out);", "void tty_get_winsize(struct tty_winsize *out);\nvoid tty_set_winsize(const struct tty_winsize *ws);")
    with open(tty_h, "w") as f:
        f.write(content)

# 2. Update tty.c
tty_c = "kernel/drivers/tty.c"
with open(tty_c, "r") as f:
    content = f.read()
if "tty_set_winsize" not in content:
    patch = """
void tty_set_winsize(const struct tty_winsize *ws) {
    irq_spin_lock(&console_tty.lock);
    console_tty.winsize = *ws;
    irq_spin_unlock(&console_tty.lock);
}
"""
    with open(tty_c, "a") as f:
        f.write(patch)

# 3. Update syscall.c
syscall_c = "kernel/proc/syscall.c"
with open(syscall_c, "r") as f:
    content = f.read()
if "TIOCSWINSZ" not in content:
    content = content.replace("#define TIOCGWINSZ 0x5413", "#define TIOCGWINSZ 0x5413\n#define TIOCSWINSZ 0x5414")
    
    ioctl_impl = """    if (request == TIOCGWINSZ) {
        if (!user_buffer_ok(p, arg, sizeof(struct tty_winsize)))
            return (uint64_t)-EFAULT;
        tty_get_winsize((struct tty_winsize *)arg);
        return 0;
    }"""
    
    new_ioctl_impl = """    if (request == TIOCGWINSZ) {
        if (!user_buffer_ok(p, arg, sizeof(struct tty_winsize)))
            return (uint64_t)-EFAULT;
        tty_get_winsize((struct tty_winsize *)arg);
        return 0;
    }
    if (request == TIOCSWINSZ) {
        if (!user_buffer_ok(p, arg, sizeof(struct tty_winsize)))
            return (uint64_t)-EFAULT;
        tty_set_winsize((const struct tty_winsize *)arg);
        return 0;
    }"""
    content = content.replace(ioctl_impl, new_ioctl_impl)
    with open(syscall_c, "w") as f:
        f.write(content)

print("Patched TIOCSWINSZ successfully")
