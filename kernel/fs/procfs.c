/*
 * /proc: a synthetic, read-only VFS filesystem.
 *
 * Nodes are made on demand (lookup_child) and die with their last reference,
 * so a directory entry for a process that has since exited can never outlive
 * its users: reads on it fail with ESRCH. File contents are generated into a
 * temporary buffer on every read and sliced by offset, which is plenty for
 * the small text files here. Everything runs in syscall context without
 * sleeping, so the process table can be inspected without further locking.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <kernel/console.h>
#include <kernel/drivers/timer.h>
#include <kernel/errno.h>
#include <kernel/fs/file.h>
#include <kernel/fs/procfs.h>
#include <kernel/fs/vfs.h>
#include <kernel/klibc/string.h>
#include <kernel/mm/kheap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/uvm.h>
#include <kernel/mm/paging.h>
#include <kernel/proc/exec.h>
#include <kernel/proc/proc.h>
#include <kernel/proc/sched.h>
#include <kernel/proc/signal.h>

#define GEN_MAX 8192
#define MAPS_MAX 32768

enum pk {
    PK_ROOT, PK_SELF,
    PK_VERSION, PK_UPTIME, PK_MEMINFO, PK_LOADAVG, PK_STAT, PK_CPUINFO,
    PK_MOUNTS, PK_FILESYSTEMS, PK_CMDLINE,
    PK_PIDDIR,
    PK_P_STAT, PK_P_STATUS, PK_P_CMDLINE, PK_P_COMM, PK_P_STATM, PK_P_ENVIRON, PK_P_MAPS,
    PK_P_EXE, PK_P_CWD, PK_P_ROOT, PK_P_FDDIR, PK_P_FD,
};

struct pnode {
    struct vfs_node node;           /* first: kfree(node) frees the whole object */
    enum pk kind;
    uint64_t pid;
    int fd;
};

struct entry { const char *name; enum pk kind; enum vfs_node_type type; };

static const struct entry top_entries[] = {
    { "self",        PK_SELF,        VFS_NODE_SYMLINK },
    { "version",     PK_VERSION,     VFS_NODE_REGULAR },
    { "uptime",      PK_UPTIME,      VFS_NODE_REGULAR },
    { "meminfo",     PK_MEMINFO,     VFS_NODE_REGULAR },
    { "loadavg",     PK_LOADAVG,     VFS_NODE_REGULAR },
    { "stat",        PK_STAT,        VFS_NODE_REGULAR },
    { "cpuinfo",     PK_CPUINFO,     VFS_NODE_REGULAR },
    { "mounts",      PK_MOUNTS,      VFS_NODE_REGULAR },
    { "filesystems", PK_FILESYSTEMS, VFS_NODE_REGULAR },
    { "cmdline",     PK_CMDLINE,     VFS_NODE_REGULAR },
};
#define TOP_COUNT (sizeof(top_entries) / sizeof(top_entries[0]))

static const struct entry pid_entries[] = {
    { "stat",    PK_P_STAT,    VFS_NODE_REGULAR },
    { "status",  PK_P_STATUS,  VFS_NODE_REGULAR },
    { "cmdline", PK_P_CMDLINE, VFS_NODE_REGULAR },
    { "comm",    PK_P_COMM,    VFS_NODE_REGULAR },
    { "statm",   PK_P_STATM,   VFS_NODE_REGULAR },
    { "environ", PK_P_ENVIRON, VFS_NODE_REGULAR },
    { "maps",    PK_P_MAPS,    VFS_NODE_REGULAR },
    { "exe",     PK_P_EXE,     VFS_NODE_SYMLINK },
    { "cwd",     PK_P_CWD,     VFS_NODE_SYMLINK },
    { "root",    PK_P_ROOT,    VFS_NODE_SYMLINK },
    { "fd",      PK_P_FDDIR,   VFS_NODE_DIRECTORY },
};
#define PID_COUNT (sizeof(pid_entries) / sizeof(pid_entries[0]))

static struct vfs_dentry root_dentry;

/* ------------------------------------------------------------------ */
/* A tiny bounded text builder (the kernel has no snprintf). */
struct out { char *buf; size_t cap, len; };

static void o_str(struct out *o, const char *s) {
    while (*s && o->len + 1 < o->cap)
        o->buf[o->len++] = *s++;
}
static void o_chr(struct out *o, char c) {
    if (o->len + 1 < o->cap) o->buf[o->len++] = c;
}
static void o_u(struct out *o, uint64_t v) {
    char t[24]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) o_chr(o, t[--n]);
}
static void o_i(struct out *o, int64_t v) {
    if (v < 0) { o_chr(o, '-'); o_u(o, (uint64_t)(-(v + 1)) + 1); }
    else o_u(o, (uint64_t)v);
}
static void o_u2(struct out *o, uint64_t v) {          /* at least two digits */
    if (v < 10) o_chr(o, '0');
    o_u(o, v);
}
static void o_hex16(struct out *o, uint64_t v) {
    for (int i = 15; i >= 0; i--) {
        int d = (int)((v >> (i * 4)) & 0xF);
        o_chr(o, (char)(d < 10 ? '0' + d : 'a' + d - 10));
    }
}
static void o_sp(struct out *o) { o_chr(o, ' '); }
static void o_nl(struct out *o) { o_chr(o, '\n'); }

/* ------------------------------------------------------------------ */
static uint64_t hz(void) { return timer_ticks_per_second(); }
static uint64_t ns_to_ticks(uint64_t ns) { return ns / (1000000000ULL / hz()); }

static uint64_t vm_bytes(struct proc *p) {
    uint64_t total = 0;
    if (p->mm)
        for (struct vma *v = p->mm->vmas; v; v = v->next)
            total += v->size;
    return total;
}

static char state_letter(struct proc *p) {
    if (p->exited) return 'Z';
    if (p->stopped) return 'T';
    for (struct thread *t = p->threads ? p->threads : &p->main_thread; t; t = t->next_in_process)
        if (t->state == THREAD_RUNNING || t->state == THREAD_RUNNABLE)
            return 'R';
    return 'S';
}

static const char *state_name(char c) {
    return c == 'R' ? "R (running)" : c == 'S' ? "S (sleeping)"
         : c == 'T' ? "T (stopped)" : "Z (zombie)";
}

static void gen_pid_stat(struct out *o, struct proc *p) {
    uint64_t utime = ns_to_ticks(p->cpu_user_ns), stime = ns_to_ticks(p->cpu_system_ns);
    uint64_t vsz = vm_bytes(p);
    o_u(o, p->pid); o_str(o, " ("); o_str(o, p->comm); o_str(o, ") ");
    o_chr(o, state_letter(p)); o_sp(o);
    o_u(o, p->parent ? p->parent->pid : 0); o_sp(o);     /* ppid */
    o_u(o, p->pgid); o_sp(o);
    o_u(o, p->sid); o_sp(o);
    o_str(o, "0 -1 ");                                   /* tty_nr, tpgid */
    o_str(o, "4194304 0 0 0 0 ");                        /* flags, min/maj faults */
    o_u(o, utime); o_sp(o); o_u(o, stime); o_sp(o);
    o_u(o, ns_to_ticks(p->children_user_ns)); o_sp(o);
    o_u(o, ns_to_ticks(p->children_system_ns)); o_sp(o);
    o_str(o, "20 0 ");                                   /* priority, nice */
    o_u(o, p->thread_count ? p->thread_count : 1); o_sp(o);
    o_str(o, "0 ");                                      /* itrealvalue */
    o_u(o, ns_to_ticks(p->start_ns)); o_sp(o);
    o_u(o, vsz); o_sp(o);
    o_u(o, vsz / 4096); o_sp(o);                         /* rss in pages (eagerly mapped) */
    o_str(o, "18446744073709551615 ");                   /* rsslim */
    /* startcode .. processor: 52 fields in total, mostly zero */
    o_str(o, "0 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0");
    o_nl(o);
}

static void sigset_line(struct out *o, const char *name, uint64_t bits) {
    o_str(o, name); o_str(o, ":\t"); o_hex16(o, bits); o_nl(o);
}

static void gen_pid_status(struct out *o, struct proc *p) {
    char st = state_letter(p);
    o_str(o, "Name:\t"); o_str(o, p->comm); o_nl(o);
    o_str(o, "Umask:\t"); o_chr(o, '0'); o_u(o, (p->file_umask >> 6) & 7);
    o_u(o, (p->file_umask >> 3) & 7); o_u(o, p->file_umask & 7); o_nl(o);
    o_str(o, "State:\t"); o_str(o, state_name(st)); o_nl(o);
    o_str(o, "Tgid:\t"); o_u(o, p->pid); o_nl(o);
    o_str(o, "Pid:\t"); o_u(o, p->pid); o_nl(o);
    o_str(o, "PPid:\t"); o_u(o, p->parent ? p->parent->pid : 0); o_nl(o);
    o_str(o, "TracerPid:\t0\n");
    o_str(o, "Uid:\t"); o_u(o, p->ruid); o_sp(o); o_u(o, p->euid); o_sp(o);
    o_u(o, p->suid); o_sp(o); o_u(o, p->euid); o_nl(o);
    o_str(o, "Gid:\t"); o_u(o, p->rgid); o_sp(o); o_u(o, p->egid); o_sp(o);
    o_u(o, p->sgid); o_sp(o); o_u(o, p->egid); o_nl(o);
    int nfd = 0;
    for (int fd = 0; fd < PROC_MAX_FDS; fd++) if (file_fd_is_open(p, fd)) nfd++;
    o_str(o, "FDSize:\t"); o_u(o, PROC_MAX_FDS); o_nl(o);
    o_str(o, "Groups:\t");
    for (size_t i = 0; i < p->supplementary_group_count; i++) { o_u(o, p->supplementary_groups[i]); o_sp(o); }
    o_nl(o);
    uint64_t vsz = vm_bytes(p) / 1024;
    o_str(o, "VmPeak:\t"); o_u(o, vsz); o_str(o, " kB\n");
    o_str(o, "VmSize:\t"); o_u(o, vsz); o_str(o, " kB\n");
    o_str(o, "VmRSS:\t"); o_u(o, vsz); o_str(o, " kB\n");
    o_str(o, "Threads:\t"); o_u(o, p->thread_count ? p->thread_count : 1); o_nl(o);
    uint64_t ign = 0, cgt = 0;
    for (int s = 1; s <= SIGNAL_MAX; s++) {
        struct signal_action a;
        if (signal_action_get(p, s, &a) != 0) continue;
        if (a.handler == SIGNAL_IGN) ign |= 1ULL << (s - 1);
        else if (a.handler > SIGNAL_IGN) cgt |= 1ULL << (s - 1);
    }
    sigset_line(o, "SigPnd", p->main_thread.signal_pending);
    sigset_line(o, "ShdPnd", p->signal_pending);
    sigset_line(o, "SigBlk", p->main_thread.signal_mask);
    sigset_line(o, "SigIgn", ign);
    sigset_line(o, "SigCgt", cgt);
    o_str(o, "voluntary_ctxt_switches:\t0\nnonvoluntary_ctxt_switches:\t0\n");
    (void)nfd;
}

/* /proc/<pid>/maps in the Linux format: "start-end perms offset dev inode  name".
 * mlibc's pthread_getattr_np() finds the stack by looking for the line that
 * contains the stack pointer. */
static void o_hex(struct out *o, uint64_t v) {
    char t[17]; int n = 0;
    do { int d = (int)(v & 0xF); t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; } while (v);
    while (n < 8) t[n++] = '0';
    while (n) o_chr(o, t[--n]);
}

static void gen_pid_maps(struct out *o, struct proc *p) {
    if (!p->mm)
        return;
    for (struct vma *v = p->mm->vmas; v; v = v->next) {
        o_hex(o, v->base); o_chr(o, '-'); o_hex(o, v->base + v->size); o_sp(o);
        o_chr(o, (v->flags & VM_READ) ? 'r' : '-');
        o_chr(o, (v->flags & VM_WRITE) ? 'w' : '-');
        o_chr(o, (v->flags & VM_EXEC) ? 'x' : '-');
        o_chr(o, v->backing ? 's' : 'p');
        o_str(o, " 00000000 00:00 0");
        if (v->base + v->size == ARCH_USER_STACK_TOP)
            o_str(o, "                          [stack]");
        o_nl(o);
    }
}

static void gen_uptime(struct out *o) {
    uint64_t up = timer_uptime_ns() / 10000000ULL;           /* centiseconds */
    size_t total = 0;
    uint64_t busy = 0;
    uint64_t pids[PROC_MAX_PROCS];
    size_t n = proc_list_pids(pids, PROC_MAX_PROCS);
    for (size_t i = 0; i < n; i++) {
        struct proc *p = proc_lookup(pids[i]);
        if (p) busy += (p->cpu_user_ns + p->cpu_system_ns) / 10000000ULL;
    }
    (void)total;
    uint64_t idle = up > busy ? up - busy : 0;
    o_u(o, up / 100); o_chr(o, '.'); o_u2(o, up % 100); o_sp(o);
    o_u(o, idle / 100); o_chr(o, '.'); o_u2(o, idle % 100); o_nl(o);
}

static void kb_line(struct out *o, const char *name, uint64_t kb) {
    o_str(o, name); o_chr(o, ':');
    size_t pad = 15 - strlen(name);
    while (pad--) o_sp(o);
    char t[24]; struct out n = { t, sizeof(t), 0 };
    o_u(&n, kb); t[n.len] = '\0';
    for (size_t i = n.len; i < 8; i++) o_sp(o);
    o_str(o, t); o_str(o, " kB\n");
}

static void gen_meminfo(struct out *o) {
    uint64_t total = pmm_total_usable_pages() * 4, free_ = pmm_free_pages() * 4;
    kb_line(o, "MemTotal", total);
    kb_line(o, "MemFree", free_);
    kb_line(o, "MemAvailable", free_);
    kb_line(o, "Buffers", 0);
    kb_line(o, "Cached", 0);
    kb_line(o, "SwapCached", 0);
    kb_line(o, "Active", total - free_);
    kb_line(o, "Inactive", 0);
    kb_line(o, "SwapTotal", 0);
    kb_line(o, "SwapFree", 0);
    kb_line(o, "Shmem", 0);
    kb_line(o, "Slab", 0);
}

static void gen_loadavg(struct out *o) {
    size_t total, running;
    proc_count_states(&total, &running, NULL);
    o_str(o, "0.00 0.00 0.00 ");
    o_u(o, running); o_chr(o, '/'); o_u(o, total); o_sp(o);
    o_u(o, proc_last_pid()); o_nl(o);
}

static void gen_stat(struct out *o) {
    uint64_t up_ticks = ns_to_ticks(timer_uptime_ns());
    uint64_t user = 0, sys = 0;
    uint64_t pids[PROC_MAX_PROCS];
    size_t n = proc_list_pids(pids, PROC_MAX_PROCS);
    for (size_t i = 0; i < n; i++) {
        struct proc *p = proc_lookup(pids[i]);
        if (p) { user += ns_to_ticks(p->cpu_user_ns); sys += ns_to_ticks(p->cpu_system_ns); }
    }
    uint64_t idle = up_ticks > user + sys ? up_ticks - user - sys : 0;
    for (int pass = 0; pass < 2; pass++) {
        o_str(o, pass ? "cpu0 " : "cpu  ");
        o_u(o, user); o_str(o, " 0 "); o_u(o, sys); o_sp(o); o_u(o, idle);
        o_str(o, " 0 0 0 0 0 0\n");
    }
    size_t total, running;
    proc_count_states(&total, &running, NULL);
    int64_t btime = timer_realtime_ns() / 1000000000LL - (int64_t)(timer_uptime_ns() / 1000000000ULL);
    o_str(o, "intr 0\nctxt 0\nbtime "); o_i(o, btime);
    o_str(o, "\nprocesses "); o_u(o, proc_last_pid());
    o_str(o, "\nprocs_running "); o_u(o, running);
    o_str(o, "\nprocs_blocked 0\n");
}

static void gen_cpuinfo(struct out *o) {
#ifdef __x86_64__
    const char *arch = "x86-64";
#else
    const char *arch = "aarch64";
#endif
    o_str(o, "processor\t: 0\nvendor_id\t: Extron\nmodel name\t: ");
    o_str(o, arch); o_str(o, " CPU\ncpu MHz\t\t: 0.000\nflags\t\t: \n\n");
}

static void gen_mounts(struct out *o) {
    struct vfs_mount_info mi;
    for (size_t i = 0; vfs_mount_info(i, &mi) == 0; i++) {
        o_str(o, mi.source); o_sp(o); o_str(o, mi.path); o_sp(o);
        o_str(o, mi.fstype); o_sp(o); o_str(o, mi.readonly ? "ro" : "rw");
        o_str(o, " 0 0\n");
    }
}

static void gen_filesystems(struct out *o) {
    o_str(o, "nodev\tproc\nnodev\tdevtmpfs\n\text2\n");
}

/* ------------------------------------------------------------------ */
static struct proc *pn_proc(const struct pnode *pn) {
    return proc_lookup(pn->pid);
}

static long gen_file(struct pnode *pn, char *buf, size_t cap) {
    struct out o = { buf, cap, 0 };
    struct proc *p = NULL;
    if (pn->kind >= PK_P_STAT && pn->kind <= PK_P_MAPS) {
        p = pn_proc(pn);
        if (!p)
            return -ESRCH;
    }
    switch (pn->kind) {
        case PK_VERSION:     o_str(&o, "Extron version 0.1 (hobby POSIX kernel) #1\n"); break;
        case PK_UPTIME:      gen_uptime(&o); break;
        case PK_MEMINFO:     gen_meminfo(&o); break;
        case PK_LOADAVG:     gen_loadavg(&o); break;
        case PK_STAT:        gen_stat(&o); break;
        case PK_CPUINFO:     gen_cpuinfo(&o); break;
        case PK_MOUNTS:      gen_mounts(&o); break;
        case PK_FILESYSTEMS: gen_filesystems(&o); break;
        case PK_CMDLINE:     o_str(&o, "extron\n"); break;
        case PK_P_STAT:      gen_pid_stat(&o, p); break;
        case PK_P_STATUS:    gen_pid_status(&o, p); break;
        case PK_P_COMM:      o_str(&o, p->comm); o_nl(&o); break;
        case PK_P_STATM: {
            uint64_t pages = vm_bytes(p) / 4096;
            o_u(&o, pages); o_sp(&o); o_u(&o, pages); o_str(&o, " 0 0 0 0 0\n");
            break;
        }
        case PK_P_CMDLINE:
            for (size_t i = 0; i < p->cmdline_len && o.len < o.cap; i++)
                o.buf[o.len++] = p->cmdline[i];
            break;
        case PK_P_ENVIRON:   break;                   /* not retained */
        case PK_P_MAPS:      gen_pid_maps(&o, p); break;
        default:             return -EINVAL;
    }
    return (long)o.len;
}

static long pn_read(struct vfs_node *n, size_t off, void *buf, size_t count) {
    struct pnode *pn = (struct pnode *)n;
    size_t cap = pn->kind == PK_P_MAPS ? MAPS_MAX : GEN_MAX;
    char *tmp = kmalloc(cap);
    if (!tmp)
        return -ENOMEM;
    long len = gen_file(pn, tmp, cap);
    long result;
    if (len < 0) {
        result = len;
    } else if (off >= (size_t)len) {
        result = 0;
    } else {
        size_t take = (size_t)len - off;
        if (take > count) take = count;
        memcpy(buf, tmp + off, take);
        result = (long)take;
    }
    kfree(tmp);
    return result;
}

static void copy_target(const char *s, void *buf, size_t size, long *out) {
    size_t l = strlen(s);
    if (l > size) l = size;
    memcpy(buf, s, l);
    *out = (long)l;
}

static long link_readlink(struct vfs_node *n, void *buf, size_t size) {
    struct pnode *pn = (struct pnode *)n;
    long out = 0;
    if (pn->kind == PK_SELF) {
        char t[24]; struct out o = { t, sizeof(t), 0 };
        o_u(&o, my_proc() ? my_proc()->pid : 0); t[o.len] = '\0';
        copy_target(t, buf, size, &out);
        return out;
    }
    struct proc *p = pn_proc(pn);
    if (!p)
        return -ESRCH;
    switch (pn->kind) {
        case PK_P_EXE:
            if (!p->exe[0]) return -ENOENT;
            copy_target(p->exe, buf, size, &out);
            return out;
        case PK_P_ROOT:
            copy_target("/", buf, size, &out);
            return out;
        case PK_P_CWD: {
            struct vfs_path cwd;
            char path[VFS_PATH_MAX + 1];
            if (proc_cwd_snapshot(p, &cwd) < 0)
                return -ENOENT;
            int r = vfs_get_path(&cwd, path, sizeof(path));
            vfs_path_release(&cwd);
            if (r < 0)
                return r;
            copy_target(path, buf, size, &out);
            return out;
        }
        case PK_P_FD: {
            char path[VFS_PATH_MAX + 1];
            int r = file_fd_describe(p, pn->fd, path, sizeof(path));
            if (r < 0)
                return -ENOENT;
            copy_target(path, buf, size, &out);
            return out;
        }
        default:
            return -EINVAL;
    }
}

/* ------------------------------------------------------------------ */
static int pn_getattr(struct vfs_node *n, struct vfs_attr *a) {
    struct pnode *pn = (struct pnode *)n;
    memset(a, 0, sizeof(*a));
    a->type = n->type;
    a->nlink = n->type == VFS_NODE_DIRECTORY ? 2 : 1;
    a->ino = pn->kind == PK_ROOT ? 1 : (pn->pid * 64 + (uint64_t)pn->kind) * 1024 + (uint64_t)(pn->fd + 1) + 2;
    switch (n->type) {
        case VFS_NODE_DIRECTORY: a->mode = 0555; break;
        case VFS_NODE_SYMLINK:   a->mode = 0777; break;
        default:                 a->mode = 0444; break;
    }
    if (pn->kind >= PK_PIDDIR && pn->kind <= PK_P_FD) {
        struct proc *p = pn_proc(pn);
        if (!p)
            return -ESRCH;
        a->uid = p->euid;
        a->gid = p->egid;
        if (pn->kind == PK_P_FDDIR) a->mode = 0500;
        if (pn->kind == PK_P_ENVIRON) a->mode = 0400;
        a->mtime.sec = a->ctime.sec = a->atime.sec = (timer_realtime_ns() / 1000000000LL);
    } else {
        a->atime.sec = a->mtime.sec = a->ctime.sec = timer_realtime_ns() / 1000000000LL;
    }
    return 0;
}

static void put_entry(struct vfs_dirent *e, uint64_t ino, enum vfs_node_type t, const char *name) {
    e->ino = ino;
    e->type = t;
    size_t l = strlen(name);
    memcpy(e->name, name, l + 1);
}

static void put_num(struct vfs_dirent *e, uint64_t ino, enum vfs_node_type t, uint64_t v) {
    char tmp[24]; struct out o = { tmp, sizeof(tmp), 0 };
    o_u(&o, v); tmp[o.len] = '\0';
    put_entry(e, ino, t, tmp);
}

static int dir_readdir(struct vfs_node *n, size_t index, struct vfs_dirent *e) {
    struct pnode *pn = (struct pnode *)n;
    e->next_cookie = 0;
    if (pn->kind == PK_ROOT) {
        if (index < TOP_COUNT) {
            put_entry(e, 100 + index, top_entries[index].type, top_entries[index].name);
            return 1;
        }
        uint64_t pids[PROC_MAX_PROCS];
        size_t cnt = proc_list_pids(pids, PROC_MAX_PROCS);
        size_t i = index - TOP_COUNT;
        if (i >= cnt)
            return 0;
        put_num(e, pids[i] * 64, VFS_NODE_DIRECTORY, pids[i]);
        return 1;
    }
    if (pn->kind == PK_PIDDIR) {
        if (!pn_proc(pn))
            return -ESRCH;
        if (index >= PID_COUNT)
            return 0;
        put_entry(e, pn->pid * 64 + index, pid_entries[index].type, pid_entries[index].name);
        return 1;
    }
    if (pn->kind == PK_P_FDDIR) {
        struct proc *p = pn_proc(pn);
        if (!p)
            return -ESRCH;
        size_t seen = 0;
        for (int fd = 0; fd < PROC_MAX_FDS; fd++) {
            if (!file_fd_is_open(p, fd))
                continue;
            if (seen++ == index) {
                put_num(e, pn->pid * 64 + 1000 + (uint64_t)fd, VFS_NODE_SYMLINK, (uint64_t)fd);
                return 1;
            }
        }
        return 0;
    }
    return -ENOTDIR;
}

static void pn_destroy(struct vfs_node *n) {
    kfree(n);                                 /* the pnode, node being its first member */
}

static const struct vfs_node_ops dir_ops  = { .readdir = dir_readdir, .getattr = pn_getattr, .destroy = pn_destroy };
static const struct vfs_node_ops file_ops = { .read = pn_read, .getattr = pn_getattr, .destroy = pn_destroy };
static const struct vfs_node_ops link_ops = { .readlink = link_readlink, .getattr = pn_getattr, .destroy = pn_destroy };
static const struct vfs_node_ops root_ops = { .readdir = dir_readdir, .getattr = pn_getattr };

static struct vfs_dentry *make_dentry(struct vfs_dentry *parent, const char *name,
                                      enum pk kind, enum vfs_node_type type,
                                      uint64_t pid, int fd) {
    struct pnode *pn = kmalloc(sizeof(*pn));
    struct vfs_dentry *d = kmalloc(sizeof(*d));
    if (!pn || !d) {
        if (pn) kfree(pn);
        if (d) kfree(d);
        return NULL;
    }
    memset(pn, 0, sizeof(*pn));
    pn->kind = kind;
    pn->pid = pid;
    pn->fd = fd;
    const struct vfs_node_ops *ops = type == VFS_NODE_DIRECTORY ? &dir_ops
                                   : type == VFS_NODE_SYMLINK   ? &link_ops : &file_ops;
    vfs_node_init(&pn->node, ops, pn, type);
    if (vfs_dentry_init(d, &pn->node, parent, name, NULL) < 0) {
        kfree(pn);
        kfree(d);
        return NULL;
    }
    vfs_node_release(&pn->node);              /* the dentry now holds the only reference */
    return d;
}

static int parse_number(const char *s, uint64_t *out) {
    if (!*s) return 0;
    uint64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10 + (uint64_t)(*s - '0');
        if (v > 0x7fffffff) return 0;
    }
    *out = v;
    return 1;
}

static int procfs_lookup_child(struct vfs_mount *mount, struct vfs_dentry *parent,
                               const char *name, struct vfs_dentry **out) {
    (void)mount;
    struct pnode *pp = (struct pnode *)parent->node;
    struct vfs_dentry *d = NULL;
    uint64_t num;
    if (pp->kind == PK_ROOT) {
        for (size_t i = 0; i < TOP_COUNT; i++)
            if (!strcmp(top_entries[i].name, name)) {
                d = make_dentry(parent, name, top_entries[i].kind, top_entries[i].type, 0, -1);
                goto done;
            }
        if (parse_number(name, &num) && proc_lookup(num))
            d = make_dentry(parent, name, PK_PIDDIR, VFS_NODE_DIRECTORY, num, -1);
        else
            return -ENOENT;
    } else if (pp->kind == PK_PIDDIR) {
        if (!proc_lookup(pp->pid))
            return -ENOENT;
        for (size_t i = 0; i < PID_COUNT; i++)
            if (!strcmp(pid_entries[i].name, name)) {
                d = make_dentry(parent, name, pid_entries[i].kind, pid_entries[i].type, pp->pid, -1);
                goto done;
            }
        return -ENOENT;
    } else if (pp->kind == PK_P_FDDIR) {
        struct proc *p = proc_lookup(pp->pid);
        if (!p || !parse_number(name, &num) || num >= PROC_MAX_FDS || !file_fd_is_open(p, (int)num))
            return -ENOENT;
        d = make_dentry(parent, name, PK_P_FD, VFS_NODE_SYMLINK, pp->pid, (int)num);
    } else {
        return -ENOTDIR;
    }
done:
    if (!d)
        return -ENOMEM;
    *out = d;
    return 0;
}

static int procfs_root(struct vfs_mount *mount, struct vfs_dentry **out) {
    (void)mount;
    vfs_dentry_retain(&root_dentry);
    *out = &root_dentry;
    return 0;
}

static int ro_create(struct vfs_mount *m, struct vfs_dentry *p, const char *n,
                     enum vfs_node_type t, uint32_t mo, uint32_t u, uint32_t g,
                     struct vfs_dentry **out) {
    (void)m; (void)p; (void)n; (void)t; (void)mo; (void)u; (void)g; (void)out;
    return -EROFS;
}
static int ro_remove(struct vfs_mount *m, struct vfs_dentry *p, const char *n, int d) {
    (void)m; (void)p; (void)n; (void)d;
    return -EROFS;
}
static int ro_rename(struct vfs_mount *m, struct vfs_dentry *op, const char *on,
                     struct vfs_dentry *np, const char *nn) {
    (void)m; (void)op; (void)on; (void)np; (void)nn;
    return -EROFS;
}
static int ro_link(struct vfs_mount *m, struct vfs_node *n, struct vfs_dentry *p,
                   const char *name, struct vfs_dentry **out) {
    (void)m; (void)n; (void)p; (void)name; (void)out;
    return -EROFS;
}
static int ro_symlink(struct vfs_mount *m, struct vfs_dentry *p, const char *name,
                      const char *target, uint32_t u, uint32_t g, struct vfs_dentry **out) {
    (void)m; (void)p; (void)name; (void)target; (void)u; (void)g; (void)out;
    return -EROFS;
}

static int procfs_statfs(struct vfs_mount *mount, struct vfs_fs_info *info) {
    (void)mount;
    memset(info, 0, sizeof(*info));
    info->block_size = 4096;
    info->fragment_size = 4096;
    info->filesystem_id = 0x70726f63ULL;          /* "proc" */
    info->flags = VFS_FS_RDONLY;
    info->name_max = VFS_NAME_MAX;
    return 0;
}

/* A cached entry for a process (or descriptor) that has since gone away must
 * not be reused. */
static int procfs_revalidate(struct vfs_dentry *d) {
    struct pnode *pn = (struct pnode *)d->node;
    if (pn->kind < PK_PIDDIR)
        return 1;
    struct proc *p = proc_lookup(pn->pid);
    if (!p)
        return 0;
    return pn->kind != PK_P_FD || file_fd_is_open(p, pn->fd);
}

static void procfs_destroy_dentry(struct vfs_dentry *d) {
    if (d != &root_dentry)
        kfree(d);
}

static const struct vfs_fs_ops procfs_fs_ops = {
    .fstype = "proc",
    .root = procfs_root,
    .lookup_child = procfs_lookup_child,
    .create = ro_create,
    .remove = ro_remove,
    .rename = ro_rename,
    .link = ro_link,
    .symlink = ro_symlink,
    .statfs = procfs_statfs,
    .destroy_dentry = procfs_destroy_dentry,
    .revalidate = procfs_revalidate,
};

void procfs_init(void) {
    static struct pnode root_pn;
    root_pn.kind = PK_ROOT;
    root_pn.fd = -1;
    vfs_node_init(&root_pn.node, &root_ops, &root_pn, VFS_NODE_DIRECTORY);
    vfs_dentry_init(&root_dentry, &root_pn.node, NULL, "", NULL);

    struct vfs_path root;
    if (vfs_root_path(&root) < 0)
        return;
    struct vfs_cred cred = { 0 };
    int r = vfs_mkdir(&root, "/proc", 0555, &cred);
    if (r != 0 && r != -EEXIST)
        kprintf("procfs: mkdir /proc failed (%d)\n", r);
    r = vfs_mount_at(&root, "/proc", &procfs_fs_ops, NULL);
    if (r != 0)
        kprintf("procfs: mount failed (%d)\n", r);
    vfs_path_release(&root);
}
