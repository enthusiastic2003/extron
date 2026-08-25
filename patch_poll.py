import sys

syscall_c = "kernel/proc/syscall.c"
with open(syscall_c, "r") as f:
    content = f.read()

start = content.find("bool waits_for_input = false;")
end = content.find("if (signal_pending_unblocked(my_thread()))")

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
