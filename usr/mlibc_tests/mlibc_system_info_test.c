#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

static void check(const char *name, int ok) {
    printf("[system_info_test] %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

static int hostname_exec_child(const char *original) {
    char name[65];
    int ok = gethostname(name, sizeof(name)) == 0
          && strcmp(name, "extron-info-test") == 0;
    if (sethostname(original, strlen(original)) != 0)
        ok = 0;
    return ok ? 0 : 71;
}

static void test_identity(const char *self) {
    puts("\n[system_info_test] === uname / hostname ===");
    struct utsname uts;
    char original[65];
    check("uname() succeeds", uname(&uts) == 0);
    check("uname reports Extron on aarch64",
          strcmp(uts.sysname, "Extron") == 0
          && strcmp(uts.machine, "aarch64") == 0);
    check("gethostname() returns uname's nodename",
          gethostname(original, sizeof(original)) == 0
          && strcmp(original, uts.nodename) == 0);

    errno = 0;
    char tiny[1];
    check("too-small hostname buffer reports ERANGE",
          gethostname(tiny, sizeof(tiny)) == -1 && errno == ERANGE);

    char too_long[65];
    memset(too_long, 'x', sizeof(too_long));
    errno = 0;
    check("hostname longer than 64 bytes is rejected",
          sethostname(too_long, sizeof(too_long)) == -1 && errno == EINVAL);

    pid_t child = fork();
    if (child == 0) {
        if (sethostname("extron-info-test", strlen("extron-info-test")) != 0)
            _exit(70);
        execl(self, self, "hostname-exec", original, (char *)0);
        _exit(72);
    }
    int status = 0;
    check("hostname is system-wide and survives exec",
          child > 0 && waitpid(child, &status, 0) == child
          && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    char restored[65];
    check("hostname test restored the original name",
          gethostname(restored, sizeof(restored)) == 0
          && strcmp(restored, original) == 0);

    child = fork();
    if (child == 0) {
        if (setresuid(1000, 1000, 1000) != 0)
            _exit(73);
        errno = 0;
        _exit(sethostname("forbidden", 9) == -1 && errno == EPERM ? 0 : 74);
    }
    status = 0;
    check("non-root sethostname() is denied",
          child > 0 && waitpid(child, &status, 0) == child
          && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_sysconf(void) {
    puts("\n[system_info_test] === sysconf / getpagesize ===");
    check("page size is the kernel's 4 KiB page",
          getpagesize() == 4096 && sysconf(_SC_PAGESIZE) == 4096);
    check("argument/environment byte ceiling is truthful",
          sysconf(_SC_ARG_MAX) == 3072);
    check("process table limit is truthful", sysconf(_SC_CHILD_MAX) == 255);
    check("scheduler clock tick rate is truthful", sysconf(_SC_CLK_TCK) == 1000);
    check("supplementary group limit is truthful", sysconf(_SC_NGROUPS_MAX) == 16);
    check("single configured/online CPU is reported",
          sysconf(_SC_NPROCESSORS_CONF) == 1
          && sysconf(_SC_NPROCESSORS_ONLN) == 1);
    check("VFS symlink expansion limit is truthful",
          sysconf(_SC_SYMLOOP_MAX) == 40);
    errno = 0;
    check("unsupported optional facilities are not advertised",
          sysconf(_SC_FSYNC) == -1 && errno == 0
          && sysconf(_SC_PRIORITY_SCHEDULING) == -1 && errno == 0
          && sysconf(_SC_CPUTIME) == -1 && errno == 0);
    check("physical page counts are plausible",
          sysconf(_SC_PHYS_PAGES) > 0
          && sysconf(_SC_AVPHYS_PAGES) > 0
          && sysconf(_SC_AVPHYS_PAGES) <= sysconf(_SC_PHYS_PAGES));

    long before = sysconf(_SC_AVPHYS_PAGES);
    void *mapping = mmap(NULL, 2 * 1024 * 1024, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    long during = sysconf(_SC_AVPHYS_PAGES);
    check("available-page count tracks eager anonymous mappings",
          mapping != MAP_FAILED && during < before);
    if (mapping != MAP_FAILED)
        munmap(mapping, 2 * 1024 * 1024);
    long after = sysconf(_SC_AVPHYS_PAGES);
    check("available pages return after munmap()",
          mapping != MAP_FAILED && after > during && after <= before);
}

static void test_pathconf(void) {
    puts("\n[system_info_test] === pathconf / fpathconf ===");
    check("pathname and filename limits match the VFS",
          pathconf("/", _PC_PATH_MAX) == PATH_MAX
          && pathconf("/", _PC_NAME_MAX) == NAME_MAX);
    check("pipe atomic-write size is reported", pathconf("/", _PC_PIPE_BUF) == PIPE_BUF);
    check("symlink target limit is reported", pathconf("/", _PC_SYMLINK_MAX) == PATH_MAX);

    errno = 0;
    check("pathconf validates the pathname",
          pathconf("/definitely/missing", _PC_NAME_MAX) == -1 && errno == ENOENT);
    errno = 0;
    check("pathconf rejects unknown selectors",
          pathconf("/", 9999) == -1 && errno == EINVAL);

    int fd = open("/", O_RDONLY | O_DIRECTORY);
    check("fpathconf works on an open descriptor without aborting",
          fd >= 0 && fpathconf(fd, _PC_NAME_MAX) == NAME_MAX);
    if (fd >= 0)
        close(fd);
    int pipefd[2] = {-1, -1};
    check("fpathconf reports PIPE_BUF for a pipe",
          pipe(pipefd) == 0 && fpathconf(pipefd[0], _PC_PIPE_BUF) == PIPE_BUF);
    if (pipefd[0] >= 0) close(pipefd[0]);
    if (pipefd[1] >= 0) close(pipefd[1]);
    errno = 0;
    check("fpathconf validates descriptors",
          fpathconf(-1, _PC_NAME_MAX) == -1 && errno == EBADF);
}

static void test_statvfs(void) {
    puts("\n[system_info_test] === statvfs / fstatvfs ===");
    struct statvfs root, by_fd, dev;
    check("statvfs reports the root filesystem",
          statvfs("/", &root) == 0 && root.f_bsize >= 1024
          && root.f_blocks > 0 && root.f_namemax == NAME_MAX);
    int fd = open("/", O_RDONLY | O_DIRECTORY);
    check("fstatvfs agrees with pathname lookup",
          fd >= 0 && fstatvfs(fd, &by_fd) == 0
          && by_fd.f_bsize == root.f_bsize
          && by_fd.f_fsid == root.f_fsid);
    if (fd >= 0) close(fd);
    check("devfs identifies itself as read-only",
          statvfs("/dev", &dev) == 0 && (dev.f_flag & ST_RDONLY));
    errno = 0;
    check("statvfs validates pathname",
          statvfs("/definitely/missing", &root) == -1 && errno == ENOENT);
    errno = 0;
    check("fstatvfs validates descriptors",
          fstatvfs(-1, &root) == -1 && errno == EBADF
          && fstatvfs(9999, &root) == -1 && errno == EBADF);

    const char *path = "/tmp/statvfs-accounting";
    unlink(path);
    check("fresh root stat succeeds before allocation", statvfs("/", &root) == 0);
    uint64_t free_before = root.f_bfree;
    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    char block[8192];
    memset(block, 0x5a, sizeof(block));
    int wrote = fd >= 0 && write(fd, block, sizeof(block)) == (ssize_t)sizeof(block);
    if (fd >= 0) close(fd);
    struct statvfs allocated;
    check("free-block count follows ext2 allocation",
          wrote && statvfs("/", &allocated) == 0
          && allocated.f_bfree < free_before);
    unlink(path);
    check("free-block count recovers after unlink",
          statvfs("/", &allocated) == 0 && allocated.f_bfree == free_before);
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "hostname-exec") == 0)
        return hostname_exec_child(argv[2]);

    puts("[system_info_test] ================================================");
    puts("[system_info_test] POSIX system information/configuration suite");
    puts("[system_info_test] ================================================");
    test_identity(argv[0]);
    test_sysconf();
    test_pathconf();
    test_statvfs();
    printf("\n[system_info_test] === %d total failure(s) ===\n", failures);
    return failures ? 1 : 0;
}
