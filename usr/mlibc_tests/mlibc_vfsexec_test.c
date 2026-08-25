/*
 * Stage 6: execve() now loads through the VFS (kernel/proc/exec.c's
 * load_binary_bytes()), not by pulling the ELF straight out of the
 * initrd tar. The only way to prove that distinction, rather than just
 * assert it, is to run a binary that never existed in the initrd at
 * all: copy an existing ELF's bytes into a brand-new ramfs file created
 * at runtime, then execve() that new path and confirm it actually runs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int failures = 0;

static void check(const char *what, int ok) {
    printf("[vfsexec_test] %-46s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    printf("\n[vfsexec_test] === execve() resolves through the VFS ===\n");

    const char *source = "/tests/mlibc_badptr_test.elf";
    const char *dest = "/tmp/vfsexec_copy.elf";

    int in = open(source, O_RDONLY);
    check("open the source ELF (already in the initrd)", in >= 0);

    struct stat st;
    check("fstat the source ELF", fstat(in, &st) == 0);

    char *buffer = malloc((size_t)st.st_size);
    check("allocate a copy buffer", buffer != NULL);

    long total = 0;
    while (buffer && total < st.st_size) {
        long n = read(in, buffer + total, (size_t)(st.st_size - total));
        if (n <= 0) break;
        total += n;
    }
    check("read the entire source ELF", total == st.st_size);
    close(in);

    int out = open(dest, O_CREAT | O_WRONLY | O_TRUNC, 0755);
    check("create a brand-new file under /tmp", out >= 0);
    long written = 0;
    while (buffer && written < total) {
        long n = write(out, buffer + written, (size_t)(total - written));
        if (n <= 0) break;
        written += n;
    }
    check("write the full copy to the new file", written == total);
    close(out);
    free(buffer);

    struct stat copy_st;
    check("the new file exists purely in ramfs, sized like the source",
          stat(dest, &copy_st) == 0 && copy_st.st_size == st.st_size);

    pid_t pid = fork();
    check("fork() before running the copy", pid >= 0);
    if (pid == 0) {
        char *args[2] = { (char *)dest, NULL };
        execve(dest, args, NULL);
        printf("[vfsexec_test]   (child) execve failed, errno set\n");
        _exit(99);
    }

    int status = -1;
    pid_t reaped = wait(&status);
    check("wait() returned the copy's pid", reaped == pid);
    check("the never-in-the-initrd copy ran and passed its own checks",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);

    printf("[vfsexec_test] === %d failure(s) ===\n", failures);
    return failures;
}
