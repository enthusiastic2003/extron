#ifndef KERNEL_FS_PROCFS_H
#define KERNEL_FS_PROCFS_H

/* Mounts a read-only synthetic filesystem at /proc (creating the mount point
 * when the root filesystem lacks it): /proc/self, /proc/<pid>/{stat,status,
 * cmdline,comm,statm,exe,cwd,root,fd/}, and uptime, meminfo, loadavg, stat,
 * cpuinfo, mounts, filesystems, version. Content is generated on every read. */
void procfs_init(void);

#endif
