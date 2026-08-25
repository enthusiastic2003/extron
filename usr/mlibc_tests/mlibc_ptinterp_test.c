/*
 * End-to-end regression for the kernel's PT_INTERP handling
 * (kernel/proc/exec.c's exec_image_build()). Executes
 * /opt/tests/mlibc_ptinterp_victim.elf, a plain static binary that
 * tools/add_pt_interp.py has spliced a PT_INTERP segment onto,
 * pointing at /opt/tests/mlibc_fake_interp.elf (see both those .c
 * files' header comments, and the Makefile rule that builds the
 * victim, for the full story — this project has no real ld.so yet, so
 * a plain static binary stands in as the "interpreter").
 *
 * If the kernel honors PT_INTERP, execve() actually starts running
 * mlibc_fake_interp.elf instead of the victim: stdout gets
 * "FAKE_INTERP_RAN\n" and the exit code is 66. If the kernel ignores
 * PT_INTERP (the pre-this-feature behavior), the victim's own main()
 * runs instead: stdout gets "VICTIM_RAN_THIS_SHOULD_NOT_HAPPEN\n" and
 * the exit code is 1. Checking both the exit code and the actual
 * output (not just one or the other) rules out a false pass from,
 * say, the kernel coincidentally producing exit code 66 some other
 * way.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int failures = 0;

static void check(const char *what, int ok) {
    printf("[ptinterp_test] %-46s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    printf("\n[ptinterp_test] === PT_INTERP is honored by the loader ===\n");

    int pipefd[2];
    check("create a pipe to capture the child's stdout", pipe(pipefd) == 0);

    pid_t pid = fork();
    check("fork() before running the patched victim", pid >= 0);

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], 1);
        close(pipefd[1]);
        char *args[2] = { (char *)"/opt/tests/mlibc_ptinterp_victim.elf", NULL };
        execve(args[0], args, NULL);
        printf("[ptinterp_test]   (child) execve failed, errno set\n");
        _exit(99);
    }

    close(pipefd[1]);

    char output[256];
    memset(output, 0, sizeof(output));
    size_t total = 0;
    for (;;) {
        long n = read(pipefd[0], output + total, sizeof(output) - 1 - total);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    close(pipefd[0]);

    int status = -1;
    pid_t reaped = wait(&status);
    check("wait() returned the victim's pid", reaped == pid);

    check("the interpreter's own message was printed, not the victim's",
          strstr(output, "FAKE_INTERP_RAN") != NULL &&
          strstr(output, "VICTIM_RAN_THIS_SHOULD_NOT_HAPPEN") == NULL);

    check("exit code is the interpreter's 66, not the victim's 1",
          WIFEXITED(status) && WEXITSTATUS(status) == 66);

    printf("[ptinterp_test] === %d failure(s) ===\n", failures);
    return failures;
}
