#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

static int failures;

static void check(const char *name, int ok) {
    printf("[file_test] %-48s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    char buf[32] = {0};
    FILE *f = fopen("hello.txt", "rb");
    check("fopen initrd file", f != NULL);
    if (f) {
        check("fread returns requested bytes", fread(buf, 1, 5, f) == 5);
        check("fread content", memcmp(buf, "Hello", 5) == 0);
        check("fseek SEEK_SET", fseek(f, 6, SEEK_SET) == 0);
        memset(buf, 0, sizeof(buf));
        check("fread after seek", fread(buf, 1, 4, f) == 4 &&
                                  memcmp(buf, "from", 4) == 0);
        check("fclose", fclose(f) == 0);
    }
    check("missing file reports failure", fopen("missing", "rb") == NULL);

    struct stat path_info, fd_info;
    FILE *metadata_file = fopen("hello.txt", "rb");
    int metadata_fd = metadata_file ? fileno(metadata_file) : -1;
    int path_stat = stat("hello.txt", &path_info);
    int path_metadata_ok = path_stat == 0 && S_ISREG(path_info.st_mode)
                         && path_info.st_size == 22 && path_info.st_ino != 0;
    if (!path_metadata_ok)
        printf("[file_test] seeded metadata: rc=%d mode=%lo size=%ld ino=%lu\n",
               path_stat, (unsigned long)path_info.st_mode,
               (long)path_info.st_size, (unsigned long)path_info.st_ino);
    check("stat returns VFS metadata for a seeded file", path_metadata_ok);
    check("fstat identifies the same VFS inode",
          metadata_fd >= 0 && fstat(metadata_fd, &fd_info) == 0
          && fd_info.st_ino == path_info.st_ino
          && fd_info.st_size == path_info.st_size);
    if (metadata_file)
        fclose(metadata_file);

    char test_directory[32];
    snprintf(test_directory, sizeof(test_directory), "file-test-%ld", (long)getpid());
    check("mkdir creates a ramfs directory", mkdir(test_directory, 0755) == 0);
    check("stat reports VFS directory type and mode",
          stat(test_directory, &path_info) == 0 && S_ISDIR(path_info.st_mode)
          && (path_info.st_mode & 0777) == 0755);

    errno = 0;
    check("duplicate mkdir reports EEXIST",
          mkdir(test_directory, 0755) == -1 && errno == EEXIST);

    char nested_a[64], nested_b[96], nested_file[128];
    snprintf(nested_a, sizeof(nested_a), "%s/a", test_directory);
    snprintf(nested_b, sizeof(nested_b), "%s/b", nested_a);
    snprintf(nested_file, sizeof(nested_file), "%s/mode-test", nested_b);
    check("mkdir creates a child directory", mkdir(nested_a, 0750) == 0);
    check("mkdir creates a nested directory", mkdir(nested_b, 0700) == 0);

    char missing_parent[96];
    snprintf(missing_parent, sizeof(missing_parent), "%s/missing/child",
             test_directory);
    errno = 0;
    check("mkdir with a missing parent reports ENOENT",
          mkdir(missing_parent, 0755) == -1 && errno == ENOENT);

    char regular_path[96], below_regular[128];
    snprintf(regular_path, sizeof(regular_path), "%s/regular", test_directory);
    snprintf(below_regular, sizeof(below_regular), "%s/child", regular_path);
    int raw_fd = open(regular_path, O_CREAT | O_EXCL | O_WRONLY, 0640);
    check("open creates a nested regular file", raw_fd >= 0);
    if (raw_fd >= 0)
        close(raw_fd);
    errno = 0;
    check("regular intermediate component reports ENOTDIR",
          mkdir(below_regular, 0755) == -1 && errno == ENOTDIR);
    errno = 0;
    raw_fd = open(regular_path, O_RDONLY | O_DIRECTORY);
    check("O_DIRECTORY on a regular file reports ENOTDIR",
          raw_fd == -1 && errno == ENOTDIR);
    if (raw_fd >= 0)
        close(raw_fd);
    errno = 0;
    raw_fd = open(regular_path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    check("O_EXCL on an existing file reports EEXIST",
          raw_fd == -1 && errno == EEXIST);
    if (raw_fd >= 0)
        close(raw_fd);
    errno = 0;
    raw_fd = open("definitely-missing-file", O_RDONLY);
    check("open missing file reports ENOENT",
          raw_fd == -1 && errno == ENOENT);
    if (raw_fd >= 0)
        close(raw_fd);

    raw_fd = open(nested_file, O_CREAT | O_EXCL | O_WRONLY, 0612);
    check("open creates a file in a nested directory", raw_fd >= 0);
    if (raw_fd >= 0)
        close(raw_fd);
    check("creation mode is retained in inode metadata",
          stat(nested_file, &path_info) == 0
          && S_ISREG(path_info.st_mode)
          && (path_info.st_mode & 0777) == 0612);

    char cwd[512], expected_cwd[128];
    snprintf(expected_cwd, sizeof(expected_cwd), "/%s/a/b", test_directory);
    check("chdir resolves a nested relative path", chdir(nested_b) == 0);
    check("getcwd reconstructs the nested path",
          getcwd(cwd, sizeof(cwd)) != NULL && !strcmp(cwd, expected_cwd));
    check("dot and dot-dot resolve by components", chdir(".././b") == 0);
    memset(buf, 0, sizeof(buf));
    FILE *from_parent = fopen("../../../hello.txt", "rb");
    check("relative lookup can walk to a parent", from_parent != NULL);
    if (from_parent) {
        check("parent-relative file has expected content",
              fread(buf, 1, 5, from_parent) == 5
              && !memcmp(buf, "Hello", 5));
        fclose(from_parent);
    }

    pid_t cwd_child = fork();
    if (cwd_child == 0)
        _Exit(chdir("/") == 0 ? 0 : 1);
    int cwd_status = 0;
    check("child can change its cwd independently",
          waitpid(cwd_child, &cwd_status, 0) == cwd_child
          && WIFEXITED(cwd_status) && WEXITSTATUS(cwd_status) == 0);
    check("parent cwd survives the child's chdir",
          getcwd(cwd, sizeof(cwd)) != NULL && !strcmp(cwd, expected_cwd));
    check("return to root cwd", chdir("/") == 0);

    char long_component_a[81], long_component_b[81];
    memset(long_component_a, 'a', sizeof(long_component_a) - 1);
    long_component_a[sizeof(long_component_a) - 1] = '\0';
    memset(long_component_b, 'b', sizeof(long_component_b) - 1);
    long_component_b[sizeof(long_component_b) - 1] = '\0';
    char long_a[160], long_b[256], expected_long[300];
    snprintf(long_a, sizeof(long_a), "%s/%s", test_directory, long_component_a);
    snprintf(long_b, sizeof(long_b), "%s/%s", long_a, long_component_b);
    snprintf(expected_long, sizeof(expected_long), "/%s", long_b);
    check("path longer than the old 100-byte limit works",
          strlen(long_b) > 100 && mkdir(long_a, 0755) == 0
          && mkdir(long_b, 0755) == 0 && chdir(long_b) == 0
          && getcwd(cwd, sizeof(cwd)) != NULL && !strcmp(cwd, expected_long));
    check("restore root after long-path test", chdir("/") == 0);

    FILE *created = fopen("default.cfg", "wb");
    check("fopen creates writable ramfs file", created != NULL);
    if (created) {
        check("fwrite ramfs file", fwrite("volume=8\n", 1, 9, created) == 9);
        check("close written ramfs file", fclose(created) == 0);
    }
    created = fopen("default.cfg", "rb");
    memset(buf, 0, sizeof(buf));
    check("reopen created file", created != NULL);
    if (created) {
        check("created file retained contents",
              fread(buf, 1, 9, created) == 9 && memcmp(buf, "volume=8\n", 9) == 0);
        fclose(created);
    }
    created = fopen("default.cfg", "wb");
    check("truncate existing file", created && fclose(created) == 0);
    created = fopen("default.cfg", "rb");
    check("truncated file is empty", created && fgetc(created) == EOF);
    if (created) fclose(created);

    FILE *shared = fopen("hello.txt", "rb");
    int fd = fileno(shared);
    char c = 0;
    check("descriptor read before fork", read(fd, &c, 1) == 1 && c == 'H');
    pid_t pid = fork();
    if (pid == 0) {
        char child = 0;
        _Exit(read(fd, &child, 1) == 1 && child == 'e' ? 0 : 1);
    }
    int status = 0;
    check("fork child consumed shared offset",
          wait(&status) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    c = 0;
    check("parent observes shared offset", read(fd, &c, 1) == 1 && c == 'l');
    fclose(shared);

    printf("[file_test] === %d failure(s) ===\n", failures);
    return failures != 0;
}
