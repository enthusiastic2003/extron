#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <grp.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int failures;

static void check(const char *name, int ok) {
    printf("[perm_test] %-55s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    char base[64], owned[96], grouped[96], private_dir[96];
    char hidden[128], sticky[96], root_victim[128], setgid_dir[96];
    char inherited[128], symlink_path[96];
    snprintf(base, sizeof(base), "/perm-test-%ld", (long)getpid());
    snprintf(owned, sizeof(owned), "%s/owned", base);
    snprintf(grouped, sizeof(grouped), "%s/grouped", base);
    snprintf(private_dir, sizeof(private_dir), "%s/private", base);
    snprintf(hidden, sizeof(hidden), "%s/hidden", private_dir);
    snprintf(sticky, sizeof(sticky), "%s/sticky", base);
    snprintf(root_victim, sizeof(root_victim), "%s/root-file", sticky);
    snprintf(setgid_dir, sizeof(setgid_dir), "%s/setgid", base);
    snprintf(inherited, sizeof(inherited), "%s/inherited", setgid_dir);
    snprintf(symlink_path, sizeof(symlink_path), "%s/link", base);

    check("boot process has root credentials",
          getuid() == 0 && geteuid() == 0 && getgid() == 0 && getegid() == 0);
    mode_t old_mask = umask(027);
    check("umask starts at 022", old_mask == 022);
    check("mkdir is filtered through umask", mkdir(base, 0777) == 0);
    struct stat st;
    check("directory creation mode became 0750",
          stat(base, &st) == 0 && (st.st_mode & 0777) == 0750);
    check("root chmod can make test directory shared", chmod(base, 0777) == 0);

    int fd = open(owned, O_CREAT | O_EXCL | O_RDWR, 0666);
    check("open creation is filtered through umask", fd >= 0);
    check("file creation mode became 0640",
          stat(owned, &st) == 0 && (st.st_mode & 0777) == 0640);
    check("fchmod changes descriptor inode mode",
          fd >= 0 && fchmod(fd, 0640) == 0
          && fstat(fd, &st) == 0 && (st.st_mode & 0777) == 0640);
    check("fchown changes descriptor inode owner and group",
          fd >= 0 && fchown(fd, 1000, 2000) == 0
          && fstat(fd, &st) == 0 && st.st_uid == 1000 && st.st_gid == 2000);
    if (fd >= 0) close(fd);

    fd = open(grouped, O_CREAT | O_EXCL | O_WRONLY, 0640);
    check("create supplementary-group test file", fd >= 0);
    if (fd >= 0) close(fd);
    check("root assigns supplementary test group", chown(grouped, 0, 2000) == 0);
    check("create non-searchable directory tree",
          mkdir(private_dir, 0700) == 0
          && (fd = open(hidden, O_CREAT | O_EXCL | O_WRONLY, 0644)) >= 0);
    if (fd >= 0) close(fd);
    check("create sticky directory", mkdir(sticky, 0777) == 0
          && chmod(sticky, 01777) == 0);
    fd = open(root_victim, O_CREAT | O_EXCL | O_WRONLY, 0644);
    check("create root-owned sticky-directory victim", fd >= 0);
    if (fd >= 0) close(fd);

    int dirfd = open(base, O_RDONLY | O_DIRECTORY);
    check("open directory descriptor", dirfd >= 0);
    fd = openat(dirfd, "relative", O_CREAT | O_EXCL | O_RDWR, 0666);
    check("openat creates relative to a directory descriptor", fd >= 0);
    if (fd >= 0) close(fd);
    check("mkdirat creates relative to a directory descriptor",
          mkdirat(dirfd, "relative-dir", 0777) == 0);
    check("faccessat resolves relative to a directory descriptor",
          faccessat(dirfd, "relative", R_OK | W_OK, AT_EACCESS) == 0);
    check("fchmodat changes a relative inode",
          fchmodat(dirfd, "relative", 0604, 0) == 0
          && fstatat(dirfd, "relative", &st, 0) == 0
          && (st.st_mode & 0777) == 0604);
    check("fchownat changes a relative inode",
          fchownat(dirfd, "relative", 1000, 2000, 0) == 0
          && fstatat(dirfd, "relative", &st, 0) == 0
          && st.st_uid == 1000 && st.st_gid == 2000);
    int cwd_fd = open(".", O_RDONLY | O_DIRECTORY);
    check("fchdir changes cwd to a directory descriptor",
          cwd_fd >= 0 && fchdir(dirfd) == 0
          && stat("relative", &st) == 0
          && fchdir(cwd_fd) == 0);
    if (cwd_fd >= 0) close(cwd_fd);
    if (dirfd >= 0) close(dirfd);

    check("create setgid directory",
          mkdir(setgid_dir, 0777) == 0
          && chown(setgid_dir, 0, 2000) == 0
          && chmod(setgid_dir, 02777) == 0);
    fd = open(inherited, O_CREAT | O_EXCL | O_WRONLY, 0666);
    check("setgid directory passes its group to new files",
          fd >= 0 && fstat(fd, &st) == 0 && st.st_gid == 2000);
    if (fd >= 0) close(fd);
    check("create symlink for no-follow metadata test",
          symlink(owned, symlink_path) == 0);
    check("fchownat with AT_SYMLINK_NOFOLLOW changes the link",
          fchownat(AT_FDCWD, symlink_path, 1234, 2345,
                   AT_SYMLINK_NOFOLLOW) == 0
          && lstat(symlink_path, &st) == 0
          && st.st_uid == 1234 && st.st_gid == 2345);

    struct timespec explicit_times[2] = {{123, 456}, {789, 123}};
    check("utimensat installs explicit access and modification times",
          utimensat(AT_FDCWD, owned, explicit_times, 0) == 0
          && stat(owned, &st) == 0
          && st.st_atim.tv_sec == 123 && st.st_atim.tv_nsec == 456
          && st.st_mtim.tv_sec == 789 && st.st_mtim.tv_nsec == 123);
    fd = open(owned, O_RDONLY);
    struct timespec now_times[2] = {{0, UTIME_NOW}, {0, UTIME_NOW}};
    check("futimens updates an opened inode",
          fd >= 0 && futimens(fd, now_times) == 0
          && fstat(fd, &st) == 0 && st.st_mtim.tv_sec != 789);
    struct timespec omitted[2] = {{0, UTIME_OMIT}, {0, UTIME_OMIT}};
    struct stat before_omit, after_omit;
    check("two UTIME_OMIT values leave all timestamps unchanged",
          fd >= 0 && fstat(fd, &before_omit) == 0
          && futimens(fd, omitted) == 0
          && fstat(fd, &after_omit) == 0
          && before_omit.st_atim.tv_sec == after_omit.st_atim.tv_sec
          && before_omit.st_atim.tv_nsec == after_omit.st_atim.tv_nsec
          && before_omit.st_mtim.tv_sec == after_omit.st_mtim.tv_sec
          && before_omit.st_mtim.tv_nsec == after_omit.st_mtim.tv_nsec
          && before_omit.st_ctim.tv_sec == after_omit.st_ctim.tv_sec
          && before_omit.st_ctim.tv_nsec == after_omit.st_ctim.tv_nsec);
    if (fd >= 0) close(fd);

    struct timespec before, after, delay = {0, 20000000};
    check("CLOCK_MONOTONIC is backed by a moving kernel clock",
          clock_gettime(CLOCK_MONOTONIC, &before) == 0
          && nanosleep(&delay, NULL) == 0
          && clock_gettime(CLOCK_MONOTONIC, &after) == 0
          && (after.tv_sec > before.tv_sec
              || (after.tv_sec == before.tv_sec
                  && after.tv_nsec > before.tv_nsec)));

    pid_t child = fork();
    if (child == 0) {
        gid_t groups[] = {2000};
        check("root child installs a supplementary group",
              setgroups(1, groups) == 0);
        check("child drops primary group", setgid(3000) == 0);
        check("child irreversibly drops user identity", setuid(1000) == 0);
        check("dropped credentials are observable",
              getuid() == 1000 && geteuid() == 1000
              && getgid() == 3000 && getegid() == 3000);
        errno = 0;
        check("non-root process cannot regain UID 0",
              setuid(0) == -1 && errno == EPERM);
        fd = open(owned, O_RDWR);
        check("owner permission grants read/write access", fd >= 0);
        if (fd >= 0) close(fd);
        fd = open(grouped, O_RDONLY);
        check("supplementary-group read permission is honored", fd >= 0);
        if (fd >= 0) close(fd);
        errno = 0;
        fd = open(grouped, O_WRONLY);
        check("supplementary group without write bit is denied",
              fd == -1 && errno == EACCES);
        if (fd >= 0) close(fd);
        errno = 0;
        check("directory search permission blocks traversal",
              stat(hidden, &st) == -1 && errno == EACCES);
        errno = 0;
        check("sticky directory protects another owner's file",
              unlink(root_victim) == -1 && errno == EPERM);
        check("file owner may chmod its own inode", chmod(owned, 0600) == 0);
        errno = 0;
        check("non-root owner may not give inode to another user",
              chown(owned, 2001, (gid_t)-1) == -1 && errno == EPERM);
        _Exit(failures ? 1 : 0);
    }
    int status = 0;
    check("privilege-drop permission checks pass in child",
          waitpid(child, &status, 0) == child
          && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    int ready[2];
    check("create synchronization pipe for signal permission test",
          pipe(ready) == 0);
    pid_t target = fork();
    if (target == 0) {
        close(ready[0]);
        int ok = setuid(2000) == 0 && write(ready[1], "R", 1) == 1;
        close(ready[1]);
        if (!ok) _Exit(2);
        for (;;) pause();
    }
    close(ready[1]);
    char marker = 0;
    check("target installs a distinct non-root identity",
          read(ready[0], &marker, 1) == 1 && marker == 'R');
    close(ready[0]);
    pid_t attacker = fork();
    if (attacker == 0) {
        int dropped = setuid(1000) == 0;
        errno = 0;
        int denied = kill(target, SIGTERM) == -1 && errno == EPERM;
        _Exit(dropped && denied ? 0 : 1);
    }
    check("one non-root UID cannot signal an unrelated UID",
          waitpid(attacker, &status, 0) == attacker
          && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    check("root can terminate the otherwise protected process",
          kill(target, SIGTERM) == 0
          && waitpid(target, &status, 0) == target
          && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);

    umask(old_mask);
    printf("[perm_test] === %d failure(s) ===\n", failures);
    return failures != 0;
}
