#include <dirent.h>
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

    char victim[96];
    snprintf(victim, sizeof(victim), "%s/open-victim", test_directory);
    raw_fd = open(victim, O_CREAT | O_EXCL | O_RDWR, 0644);
    check("create file for open-unlink lifetime", raw_fd >= 0);
    check("write file before unlink",
          raw_fd >= 0 && write(raw_fd, "kept", 4) == 4
          && lseek(raw_fd, 0, SEEK_SET) == 0);
    check("unlink removes a regular pathname", unlink(victim) == 0);
    errno = 0;
    check("unlinked pathname reports ENOENT",
          stat(victim, &path_info) == -1 && errno == ENOENT);
    memset(buf, 0, sizeof(buf));
    check("open descriptor survives unlink",
          raw_fd >= 0 && read(raw_fd, buf, 4) == 4
          && !memcmp(buf, "kept", 4));
    check("unlinked open inode reports zero links",
          raw_fd >= 0 && fstat(raw_fd, &fd_info) == 0 && fd_info.st_nlink == 0);
    if (raw_fd >= 0)
        close(raw_fd);
    errno = 0;
    check("unlink missing pathname reports ENOENT",
          unlink(victim) == -1 && errno == ENOENT);
    errno = 0;
    check("unlink directory reports EISDIR",
          unlink(nested_a) == -1 && errno == EISDIR);
    errno = 0;
    check("rmdir regular file reports ENOTDIR",
          rmdir(regular_path) == -1 && errno == ENOTDIR);
    errno = 0;
    check("rmdir non-empty directory reports ENOTEMPTY",
          rmdir(nested_b) == -1 && errno == ENOTEMPTY);

    char empty_dir[96];
    snprintf(empty_dir, sizeof(empty_dir), "%s/empty", test_directory);
    check("create empty directory for rmdir", mkdir(empty_dir, 0755) == 0);
    DIR *open_directory = opendir(empty_dir);
    check("open directory before rmdir", open_directory != NULL);
    check("rmdir removes an empty directory", rmdir(empty_dir) == 0);
    errno = 0;
    check("removed directory pathname reports ENOENT",
          stat(empty_dir, &path_info) == -1 && errno == ENOENT);
    errno = 0;
    check("open directory descriptor survives rmdir",
          open_directory && readdir(open_directory) == NULL && errno == 0);
    if (open_directory)
        check("close removed directory descriptor", closedir(open_directory) == 0);

    char rename_source[96], rename_target[96];
    snprintf(rename_source, sizeof(rename_source), "%s/rename-source",
             test_directory);
    snprintf(rename_target, sizeof(rename_target), "%s/rename-target",
             test_directory);
    int source_fd = open(rename_source, O_CREAT | O_EXCL | O_RDWR, 0644);
    int old_target_fd = open(rename_target, O_CREAT | O_EXCL | O_RDWR, 0644);
    check("create files for replacement rename",
          source_fd >= 0 && old_target_fd >= 0);
    check("seed both rename generations",
          source_fd >= 0 && old_target_fd >= 0
          && write(source_fd, "new", 3) == 3
          && write(old_target_fd, "old", 3) == 3
          && lseek(old_target_fd, 0, SEEK_SET) == 0);
    if (source_fd >= 0)
        close(source_fd);
    check("rename atomically replaces a regular target",
          rename(rename_source, rename_target) == 0);
    errno = 0;
    check("renamed source pathname disappears",
          stat(rename_source, &path_info) == -1 && errno == ENOENT);
    int renamed_fd = open(rename_target, O_RDONLY);
    memset(buf, 0, sizeof(buf));
    check("replacement pathname names source contents",
          renamed_fd >= 0 && read(renamed_fd, buf, 3) == 3
          && !memcmp(buf, "new", 3));
    if (renamed_fd >= 0)
        close(renamed_fd);
    memset(buf, 0, sizeof(buf));
    check("open replaced inode retains old contents",
          old_target_fd >= 0 && read(old_target_fd, buf, 3) == 3
          && !memcmp(buf, "old", 3));
    if (old_target_fd >= 0)
        close(old_target_fd);
    check("rename of a pathname to itself is a no-op",
          rename(rename_target, rename_target) == 0);
    errno = 0;
    check("rename missing source reports ENOENT",
          rename(rename_source, victim) == -1 && errno == ENOENT);

    char type_dir[96];
    snprintf(type_dir, sizeof(type_dir), "%s/type-dir", test_directory);
    check("create directory for rename type checks", mkdir(type_dir, 0755) == 0);
    errno = 0;
    check("regular file cannot replace a directory",
          rename(regular_path, type_dir) == -1 && errno == EISDIR);
    errno = 0;
    check("directory cannot replace a regular file",
          rename(type_dir, regular_path) == -1 && errno == ENOTDIR);

    char replace_dir_source[96], replace_dir_target[96];
    snprintf(replace_dir_source, sizeof(replace_dir_source), "%s/dir-source",
             test_directory);
    snprintf(replace_dir_target, sizeof(replace_dir_target), "%s/dir-target",
             test_directory);
    check("create empty directories for replacement rename",
          mkdir(replace_dir_source, 0755) == 0
          && mkdir(replace_dir_target, 0755) == 0);
    check("directory rename replaces an empty directory",
          rename(replace_dir_source, replace_dir_target) == 0);

    char nonempty_source[96], nonempty_target[96], nonempty_child[128];
    snprintf(nonempty_source, sizeof(nonempty_source), "%s/nonempty-source",
             test_directory);
    snprintf(nonempty_target, sizeof(nonempty_target), "%s/nonempty-target",
             test_directory);
    snprintf(nonempty_child, sizeof(nonempty_child), "%s/child", nonempty_target);
    check("create directories for non-empty replacement check",
          mkdir(nonempty_source, 0755) == 0
          && mkdir(nonempty_target, 0755) == 0
          && mkdir(nonempty_child, 0755) == 0);
    errno = 0;
    check("rename cannot replace a non-empty directory",
          rename(nonempty_source, nonempty_target) == -1
          && errno == ENOTEMPTY);

    char move_source[96], move_child[128], move_target[128];
    snprintf(move_source, sizeof(move_source), "%s/move-source", test_directory);
    snprintf(move_child, sizeof(move_child), "%s/child", move_source);
    snprintf(move_target, sizeof(move_target), "%s/moved", nested_a);
    check("create directory tree for cross-parent rename",
          mkdir(move_source, 0755) == 0 && mkdir(move_child, 0755) == 0);
    check("enter directory before its ancestor is renamed",
          chdir(move_child) == 0);
    char absolute_source[128], absolute_target[160], expected_moved[192];
    snprintf(absolute_source, sizeof(absolute_source), "/%s", move_source);
    snprintf(absolute_target, sizeof(absolute_target), "/%s", move_target);
    snprintf(expected_moved, sizeof(expected_moved), "/%s/child", move_target);
    check("rename moves a populated directory across parents",
          rename(absolute_source, absolute_target) == 0);
    check("cwd follows a renamed ancestor",
          getcwd(cwd, sizeof(cwd)) != NULL && !strcmp(cwd, expected_moved));
    char descendant_target[224];
    snprintf(descendant_target, sizeof(descendant_target), "%s/child/inside",
             absolute_target);
    errno = 0;
    check("directory cannot be renamed into its descendant",
          rename(absolute_target, descendant_target) == -1 && errno == EINVAL);
    check("return to root after rename tests", chdir("/") == 0);

    char cwd_removed[96];
    snprintf(cwd_removed, sizeof(cwd_removed), "%s/cwd-removed", test_directory);
    check("create directory for retained removed cwd",
          mkdir(cwd_removed, 0755) == 0 && chdir(cwd_removed) == 0);
    char absolute_removed[128];
    snprintf(absolute_removed, sizeof(absolute_removed), "/%s", cwd_removed);
    check("rmdir can detach a directory still used as cwd",
          rmdir(absolute_removed) == 0);
    errno = 0;
    check("getcwd reports detached cwd as ENOENT",
          getcwd(cwd, sizeof(cwd)) == NULL && errno == ENOENT);
    check("dot-dot escapes a detached cwd", chdir("..") == 0);
    snprintf(expected_cwd, sizeof(expected_cwd), "/%s", test_directory);
    check("cwd recovers at retained parent",
          getcwd(cwd, sizeof(cwd)) != NULL && !strcmp(cwd, expected_cwd));
    check("restore root after detached-cwd test", chdir("/") == 0);
    check("access accepts an existing path while permissions are deferred",
          access("hello.txt", R_OK | W_OK) == 0);
    errno = 0;
    check("access missing path reports ENOENT",
          access("access-missing", F_OK) == -1 && errno == ENOENT);
    errno = 0;
    check("rmdir root reports EBUSY", rmdir("/") == -1 && errno == EBUSY);

    char symlink_path[96], relative_link[96], hardlink_path[96];
    snprintf(symlink_path, sizeof(symlink_path), "%s/hello-link", test_directory);
    snprintf(relative_link, sizeof(relative_link), "%s/relative-link", nested_a);
    snprintf(hardlink_path, sizeof(hardlink_path), "%s/hard-link", test_directory);
    check("symlink creates a symbolic link",
          symlink("/hello.txt", symlink_path) == 0);
    char link_buffer[64] = {0};
    check("readlink returns the stored target without a terminator",
          readlink(symlink_path, link_buffer, sizeof(link_buffer)) == 10
          && !memcmp(link_buffer, "/hello.txt", 10));
    check("lstat identifies the symlink itself",
          lstat(symlink_path, &path_info) == 0 && S_ISLNK(path_info.st_mode)
          && path_info.st_size == 10);
    check("stat follows the symlink",
          stat(symlink_path, &path_info) == 0 && S_ISREG(path_info.st_mode)
          && path_info.st_size == 22);
    errno = 0;
    raw_fd = open(symlink_path, O_RDONLY | O_NOFOLLOW);
    check("O_NOFOLLOW rejects a final symbolic link with ELOOP",
          raw_fd == -1 && errno == ELOOP);
    if (raw_fd >= 0)
        close(raw_fd);
    char symlink_slash[104];
    snprintf(symlink_slash, sizeof(symlink_slash), "%s/", symlink_path);
    errno = 0;
    check("trailing slash follows a symlink and requires a directory",
          lstat(symlink_slash, &path_info) == -1 && errno == ENOTDIR);
    check("relative symlink target is based at link parent",
          symlink("../../hello.txt", relative_link) == 0
          && stat(relative_link, &path_info) == 0 && path_info.st_size == 22);
    check("hard link creates another name for one inode",
          link(rename_target, hardlink_path) == 0
          && stat(rename_target, &path_info) == 0
          && stat(hardlink_path, &fd_info) == 0
          && path_info.st_ino == fd_info.st_ino && path_info.st_nlink == 2);
    check("hard link survives unlink of the original name",
          unlink(rename_target) == 0
          && stat(hardlink_path, &path_info) == 0 && path_info.st_nlink == 1);
    errno = 0;
    check("hard-linking a directory reports EPERM",
          link(nested_a, victim) == -1 && errno == EPERM);
    char loop_a[96], loop_b[96];
    snprintf(loop_a, sizeof(loop_a), "%s/loop-a", test_directory);
    snprintf(loop_b, sizeof(loop_b), "%s/loop-b", test_directory);
    check("create a symbolic-link loop",
          symlink("loop-b", loop_a) == 0 && symlink("loop-a", loop_b) == 0);
    errno = 0;
    check("symlink loop reports ELOOP",
          stat(loop_a, &path_info) == -1 && errno == ELOOP);

    char at_source[128], at_empty[128];
    snprintf(at_source, sizeof(at_source), "%s/at-source", nested_a);
    snprintf(at_empty, sizeof(at_empty), "%s/at-empty", nested_a);
    int directory_fd = open(nested_a, O_RDONLY | O_DIRECTORY);
    check("open directory fd for *at operations", directory_fd >= 0);
    int at_source_fd = open(at_source, O_CREAT | O_EXCL | O_WRONLY, 0644);
    check("create source for directory-fd operations", at_source_fd >= 0);
    if (at_source_fd >= 0)
        close(at_source_fd);
    check("renameat resolves both relative paths from directory fds",
          directory_fd >= 0
          && renameat(directory_fd, "at-source",
                      directory_fd, "at-renamed") == 0);
    check("symlinkat stores a relative target under a directory fd",
          directory_fd >= 0
          && symlinkat("../../hello.txt", directory_fd, "at-symlink") == 0);
    memset(link_buffer, 0, sizeof(link_buffer));
    check("readlinkat resolves a relative path from a directory fd",
          directory_fd >= 0
          && readlinkat(directory_fd, "at-symlink", link_buffer,
                        sizeof(link_buffer)) == 15
          && !memcmp(link_buffer, "../../hello.txt", 15));
    check("fstatat can inspect rather than follow a symbolic link",
          directory_fd >= 0
          && fstatat(directory_fd, "at-symlink", &path_info,
                     AT_SYMLINK_NOFOLLOW) == 0
          && S_ISLNK(path_info.st_mode));
    check("fstatat follows symbolic links by default",
          directory_fd >= 0
          && fstatat(directory_fd, "at-symlink", &path_info, 0) == 0
          && S_ISREG(path_info.st_mode) && path_info.st_size == 22);
    check("linkat creates a hard link relative to directory fds",
          directory_fd >= 0
          && linkat(directory_fd, "at-renamed",
                    directory_fd, "at-hard", 0) == 0
          && fstatat(directory_fd, "at-renamed", &path_info, 0) == 0
          && fstatat(directory_fd, "at-hard", &fd_info, 0) == 0
          && path_info.st_ino == fd_info.st_ino && path_info.st_nlink == 2);
    check("linkat without AT_SYMLINK_FOLLOW links the symlink itself",
          directory_fd >= 0
          && linkat(directory_fd, "at-symlink",
                    directory_fd, "at-link-to-link", 0) == 0
          && fstatat(directory_fd, "at-link-to-link", &path_info,
                     AT_SYMLINK_NOFOLLOW) == 0
          && S_ISLNK(path_info.st_mode));
    check("linkat with AT_SYMLINK_FOLLOW links the symlink target",
          directory_fd >= 0
          && linkat(directory_fd, "at-symlink",
                    directory_fd, "at-link-to-target",
                    AT_SYMLINK_FOLLOW) == 0
          && fstatat(directory_fd, "at-link-to-target", &path_info,
                     AT_SYMLINK_NOFOLLOW) == 0
          && S_ISREG(path_info.st_mode));
    check("unlinkat removes a file relative to a directory fd",
          directory_fd >= 0
          && unlinkat(directory_fd, "at-renamed", 0) == 0);
    check("create empty directory for unlinkat AT_REMOVEDIR",
          mkdir(at_empty, 0755) == 0);
    check("unlinkat AT_REMOVEDIR removes a relative directory",
          directory_fd >= 0
          && unlinkat(directory_fd, "at-empty", AT_REMOVEDIR) == 0);
    check("absolute *at paths ignore the supplied directory fd",
          fstatat(-999, "/hello.txt", &path_info, 0) == 0
          && S_ISREG(path_info.st_mode));
    int nondirectory_fd = open(regular_path, O_RDONLY);
    errno = 0;
    check("relative *at path with a non-directory fd reports ENOTDIR",
          nondirectory_fd >= 0
          && fstatat(nondirectory_fd, "child", &path_info, 0) == -1
          && errno == ENOTDIR);
    if (nondirectory_fd >= 0)
        close(nondirectory_fd);
    errno = 0;
    check("relative *at path with an invalid fd reports EBADF",
          fstatat(-999, "child", &path_info, 0) == -1 && errno == EBADF);
    if (directory_fd >= 0)
        close(directory_fd);

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
