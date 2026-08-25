import sys

generic_cpp = "third_party/mlibc/sysdeps/extron/generic/generic.cpp"
with open(generic_cpp, "a") as f:
    f.write("""
int sys_unlockpt(int fd) {
    int zero = 0;
    return sys_ioctl(fd, 0x40045431 /* TIOCSPTLCK */, &zero);
}

int sys_ptsname(int fd, char *buffer, size_t length) {
    int pty_num;
    if (sys_ioctl(fd, 0x80045430 /* TIOCGPTN */, &pty_num) != 0) {
        return ENOTTY;
    }
    
    // Extron DevFS dynamic nodes are at /dev/ptsN
    // Format the path string manually since snprintf might not be fully linked here?
    // Actually, mlibc has snprintf. But it's a sysdep so we can just use manual strcpy.
    const char *prefix = "/dev/pts";
    if (length < 16) return ERANGE;
    
    int i = 0;
    while (prefix[i]) { buffer[i] = prefix[i]; i++; }
    
    // Convert number to string
    if (pty_num == 0) {
        buffer[i++] = '0';
    } else {
        int temp = pty_num;
        int num_digits = 0;
        while (temp > 0) { temp /= 10; num_digits++; }
        temp = pty_num;
        for (int j = num_digits - 1; j >= 0; j--) {
            buffer[i + j] = '0' + (temp % 10);
            temp /= 10;
        }
        i += num_digits;
    }
    buffer[i] = '\\0';
    return 0;
}
""")
