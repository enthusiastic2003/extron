/*
 * setresuid()/setresgid() (kernel/proc/syscall.c's sys_setresuid()/
 * sys_setresgid()) set the real/effective/saved triple independently,
 * -1 in any slot meaning "leave that one alone" — the precise tool a
 * privilege-drop pattern needs and setuid()/seteuid() (already covered
 * by mlibc_perm_test.c) can't express: neither lets a non-root process
 * choose its OWN real id independently of its effective one, or park a
 * value in the saved id it can later setresuid() back to.
 *
 * The interesting failure mode is a PARTIAL apply — some requested IDs
 * changing while others are rejected, leaving a process in a set of
 * credentials nobody actually asked for. All three requests are checked
 * against the pre-call triple before anything is written, so a run in
 * a forked, disposable child is what proves "all or nothing" here: a
 * child that ends up with a mismatched, unintended identity would still
 * report its own EPERM correctly, but no observer outside the crash
 * itself would know the write happened at all.
 */
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

static int failures = 0;

static void check(const char *what, int ok) {
    printf("[setresid_test] %-46s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int run_as_child(void) {
    check("boot process starts fully root (setresgid no-op)",
          setresgid((gid_t)-1, (gid_t)-1, (gid_t)-1) == 0
          && getgid() == 0 && getegid() == 0);

    /* Groups first, while still root: setresgid()'s privilege check is
     * euid == 0, same as everywhere else credentials are checked here,
     * so an arbitrary group triple only succeeds BEFORE the euid drop
     * below — the reason real privilege-dropping code sets its group
     * identity before its user identity, not after. */
    check("root can set an arbitrary group triple",
          setresgid(4000, 5000, 6000) == 0
          && getgid() == 4000 && getegid() == 5000);

    check("root can set an arbitrary, distinct r/e/s triple",
          setresuid(1000, 2000, 3000) == 0
          && getuid() == 1000 && geteuid() == 2000);

    /* From here euid is 2000: every check below runs genuinely
     * unprivileged, for both the user and the group triple — a gid
     * change no longer bypasses anything just because it hasn't
     * touched uid, since the kernel's only privilege signal is euid. */
    errno = 0;
    check("dropped identity cannot regain root", setresuid(0, 0, 0) == -1
          && errno == EPERM);

    check("effective id can move to the OLD saved id",
          setresuid((uid_t)-1, 3000, (uid_t)-1) == 0
          && geteuid() == 3000 && getuid() == 1000);

    errno = 0;
    check("a value outside the current triple is rejected",
          setresuid((uid_t)-1, 9999, (uid_t)-1) == -1 && errno == EPERM
          && geteuid() == 3000);

    errno = 0;
    check("unprivileged euid also blocks reclaiming root group",
          setresgid(0, 0, 0) == -1 && errno == EPERM);

    check("effective gid can move to the old saved gid",
          setresgid((gid_t)-1, 6000, (gid_t)-1) == 0 && getegid() == 6000);

    errno = 0;
    check("a group value outside the current triple is rejected",
          setresgid((gid_t)-1, 9999, (gid_t)-1) == -1 && errno == EPERM
          && getegid() == 6000);

    return failures;
}

int main(void) {
    printf("\n[setresid_test] === setresuid()/setresgid() r/e/s triple ===\n");

    pid_t pid = fork();
    check("fork() before permanently dropping identity", pid >= 0);
    if (pid == 0)
        _exit(run_as_child());

    int status = -1;
    pid_t reaped = wait(&status);
    check("wait() returned the child's pid", reaped == pid);
    check("child's own checks all passed",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);

    printf("[setresid_test] === %d failure(s) ===\n", failures);
    return failures;
}
