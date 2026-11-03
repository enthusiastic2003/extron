/* /proc: self, per-process files, system files, lifetime of pid entries,
 * read-only semantics, sysinfo(). */
#define _GNU_SOURCE
#include "arch_compat.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>

static int failures;
static void check(const char *what, int ok) {
    printf("[proc_test] %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int slurp(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t n = 0;
    for (;;) {
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = '\0';
    return (int)n;
}

static int has_pid_entry(pid_t pid) {
    DIR *d = opendir("/proc");
    int found = 0;
    char want[16];
    snprintf(want, sizeof want, "%d", (int)pid);
    for (struct dirent *e; d && (e = readdir(d));)
        if (!strcmp(e->d_name, want)) found = 1;
    if (d) closedir(d);
    return found;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char buf[4096], path[96];
    struct stat st;

    check("/proc is a directory", stat("/proc", &st) == 0 && S_ISDIR(st.st_mode));

    /* ---- /proc/self ---- */
    char target[32] = {0};
    ssize_t n = readlink("/proc/self", target, sizeof target - 1);
    check("/proc/self links to our pid", n > 0 && atoi(target) == (int)getpid());
    check("our pid is listed in /proc", has_pid_entry(getpid()));

    char comm[32] = {0};
    check("/proc/self/comm is our program name",
          slurp("/proc/self/comm", comm, sizeof comm) > 0 && strstr(comm, "proc_test"));

    /* stat: "pid (comm) S ppid pgrp session ..." */
    check("/proc/self/stat is readable", slurp("/proc/self/stat", buf, sizeof buf) > 0);
    int pid = 0, ppid = 0, pgrp = 0, sid = 0; char state = 0, name[32] = {0};
    char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
    int ok = lp && rp && rp > lp;
    if (ok) {
        pid = atoi(buf);
        memcpy(name, lp + 1, (size_t)(rp - lp - 1) < 31 ? (size_t)(rp - lp - 1) : 31);
        sscanf(rp + 2, "%c %d %d %d", &state, &ppid, &pgrp, &sid);
    }
    check("stat: pid and comm", ok && pid == getpid() && strstr(name, "proc_test"));
    check("stat: running state", state == 'R');
    check("stat: ppid/pgrp/session match libc",
          ppid == getppid() && pgrp == getpgrp() && sid == getsid(0));
    unsigned long vsize = 0;
    {   /* vsize is field 23: skip 20 fields after state */
        char *p = rp + 2; int field = 3;
        while (*p && field < 23) { if (*p == ' ') field++; p++; }
        vsize = strtoul(p, NULL, 10);
    }
    check("stat: vsize is plausible", vsize > 100000 && vsize < (1UL << 32));

    /* status */
    check("/proc/self/status readable", slurp("/proc/self/status", buf, sizeof buf) > 0);
    char want[48];
    snprintf(want, sizeof want, "Pid:\t%d", (int)getpid());
    check("status: Pid line", strstr(buf, want) != NULL);
    snprintf(want, sizeof want, "Uid:\t%d", (int)getuid());
    check("status: Uid line", strstr(buf, want) != NULL);
    check("status: State line", strstr(buf, "State:\tR (running)") != NULL);

    /* cmdline = argv, NUL separated */
    n = slurp("/proc/self/cmdline", buf, sizeof buf);
    check("cmdline starts with argv[0]", n > 0 && !strcmp(buf, argv[0]));
    (void)argc;

    /* exe and cwd */
    n = readlink("/proc/self/exe", target, sizeof target - 1);
    check("exe is the program path", n > 0 && (target[n] = 0, strstr(target, "proc_test") != NULL));
    char cwdbuf[96] = {0}, procwd[96] = {0};
    chdir("/tmp");
    n = readlink("/proc/self/cwd", procwd, sizeof procwd - 1);
    check("cwd link follows chdir", n > 0 && !strcmp(procwd, "/tmp") && getcwd(cwdbuf, sizeof cwdbuf));
    chdir("/");

    /* fd directory */
    int fd = open("/opt/tests/hello.txt", O_RDONLY);
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    char fdtarget[128] = {0};
    n = readlink(path, fdtarget, sizeof fdtarget - 1);
    check("fd/N links to the opened file's path",
          n > 0 && strstr(fdtarget, "hello.txt") != NULL);
    DIR *d = opendir("/proc/self/fd");
    int saw0 = 0, sawfd = 0;
    char fdname[16];
    snprintf(fdname, sizeof fdname, "%d", fd);
    for (struct dirent *e; d && (e = readdir(d));) {
        if (!strcmp(e->d_name, "0")) saw0 = 1;
        if (!strcmp(e->d_name, fdname)) sawfd = 1;
    }
    if (d) closedir(d);
    check("fd directory lists open descriptors", saw0 && sawfd);
    close(fd);
    errno = 0;
    check("closed fd disappears", readlink(path, fdtarget, sizeof fdtarget) == -1 && errno == ENOENT);
    int pfd[2]; pipe(pfd);
    snprintf(path, sizeof path, "/proc/self/fd/%d", pfd[0]);
    n = readlink(path, fdtarget, sizeof fdtarget - 1);
    check("pipe fds show as pipe:[...]", n > 0 && !strncmp(fdtarget, "pipe:", 5));
    close(pfd[0]); close(pfd[1]);

    /* ---- lifetime of another process's entries ---- */
    int sync_pipe[2]; pipe(sync_pipe);
    pid_t child = fork();
    if (child == 0) {
        char c; read(sync_pipe[0], &c, 1);
        _exit(0);
    }
    check("child is listed while alive", has_pid_entry(child));
    snprintf(path, sizeof path, "/proc/%d/stat", (int)child);
    check("child's stat is readable", slurp(path, buf, sizeof buf) > 0 && atoi(buf) == (int)child);
    snprintf(path, sizeof path, "/proc/%d/status", (int)child);
    slurp(path, buf, sizeof buf);
    snprintf(want, sizeof want, "PPid:\t%d", (int)getpid());
    check("child's status shows us as parent", strstr(buf, want) != NULL);
    snprintf(path, sizeof path, "/proc/%d/stat", (int)child);
    int cfd = open(path, O_RDONLY);
    write(sync_pipe[1], "x", 1);
    int status; waitpid(child, &status, 0);
    check("reaped child leaves /proc", !has_pid_entry(child));
    errno = 0;
    check("opening a dead pid's file fails", open(path, O_RDONLY) == -1 && errno == ENOENT);
    if (cfd >= 0) {
        errno = 0;
        ssize_t r = read(cfd, buf, sizeof buf);
        check("a handle opened earlier now reports ESRCH", r == -1 && errno == ESRCH);
        close(cfd);
    }
    close(sync_pipe[0]); close(sync_pipe[1]);

    /* ---- system files ---- */
    check("/proc/uptime", slurp("/proc/uptime", buf, sizeof buf) > 0);
    double up1 = atof(buf);
    usleep(300000);
    slurp("/proc/uptime", buf, sizeof buf);
    check("uptime advances", atof(buf) > up1 + 0.2);

    slurp("/proc/meminfo", buf, sizeof buf);
    unsigned long mt = 0, mf = 0;
    char *p = strstr(buf, "MemTotal:"); if (p) mt = strtoul(p + 9, NULL, 10);
    p = strstr(buf, "MemFree:"); if (p) mf = strtoul(p + 8, NULL, 10);
    check("meminfo: MemTotal/MemFree sane", mt > 65536 && mf > 0 && mf < mt);

    slurp("/proc/loadavg", buf, sizeof buf);
    check("loadavg format", strstr(buf, "0.00") != NULL && strchr(buf, '/') != NULL);
    slurp("/proc/stat", buf, sizeof buf);
    check("stat: cpu line and btime", !strncmp(buf, "cpu ", 4) && strstr(buf, "btime ") != NULL);
    slurp("/proc/mounts", buf, sizeof buf);
    check("mounts lists / and /proc", strstr(buf, " / ") != NULL && strstr(buf, " /proc proc ") != NULL);
    check("version and cpuinfo exist", slurp("/proc/version", buf, sizeof buf) > 0
          && slurp("/proc/cpuinfo", buf, sizeof buf) > 0 && strstr(buf, "processor") != NULL);
    check("filesystems lists ext2", slurp("/proc/filesystems", buf, sizeof buf) > 0 && strstr(buf, "ext2"));

    struct sysinfo si;
    check("sysinfo() works", sysinfo(&si) == 0 && si.totalram > 0 && si.freeram > 0
          && si.freeram <= si.totalram && si.procs >= 1 && si.mem_unit > 0);

    /* ---- read-only ---- */
    errno = 0;
    check("cannot create files in /proc", open("/proc/newfile", O_CREAT | O_WRONLY, 0644) == -1);
    errno = 0;
    check("cannot mkdir in /proc", mkdir("/proc/newdir", 0755) == -1 && errno == EROFS);
    int w = open("/proc/version", O_WRONLY);
    errno = 0;
    check("writing a /proc file fails", w < 0 || write(w, "x", 1) == -1);
    if (w >= 0) close(w);
    check("stat of a proc file: regular, no write bits",
          stat("/proc/version", &st) == 0 && S_ISREG(st.st_mode) && !(st.st_mode & 0222));

    printf("[proc_test] === %d failure(s) ===\n", failures);
    return failures ? 1 : 0;
}
