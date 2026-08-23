sysdep_supported_options = {
	'posix': true,
	'linux': false,
	'glibc': true,
	'bsd': false,
}

add_project_arguments('-D_GNU_SOURCE', language: ['c', 'cpp'])

# Add any hardware capabilities you are missing here
# libc_hw_caps += {
#    'missing_signal_semantics': true,
#    'missing_file_locking': true,
#}

# Compile our generic.cpp into both the dynamic linker and the standard library
# rtld_sources += files('generic/generic.cpp')
libc_sources += files('generic/generic.cpp', 'generic/thread.cpp', 'thread_entry.S')

# rtld_include_dirs += include_directories('include')
libc_include_dirs += include_directories('include')

if not no_headers
    install_headers('include/sys/sysmacros.h', subdir: 'sys')
    install_headers(
        'include/abi-bits/access.h',
        'include/abi-bits/auxv.h',
        'include/abi-bits/blkcnt_t.h',
        'include/abi-bits/blksize_t.h',
        'include/abi-bits/clockid_t.h',
        'include/abi-bits/dev_t.h',
        'include/abi-bits/epoll.h',
        'include/abi-bits/errno.h',
        'include/abi-bits/fcntl.h',
        'include/abi-bits/fsblkcnt_t.h',
        'include/abi-bits/fsfilcnt_t.h',
        'include/abi-bits/gid_t.h',
        'include/abi-bits/in.h',
        'include/abi-bits/ino_t.h',
        'include/abi-bits/inotify.h',
        'include/abi-bits/ioctls.h',
        'include/abi-bits/ipc.h',
        'include/abi-bits/limits.h',
        'include/abi-bits/mode_t.h',
        'include/abi-bits/mqueue.h',
        'include/abi-bits/msg.h',
        'include/abi-bits/nlink_t.h',
        'include/abi-bits/packet.h',
        'include/abi-bits/pid_t.h',
        'include/abi-bits/poll.h',
        'include/abi-bits/ptrace.h',
        'include/abi-bits/random.h',
        'include/abi-bits/reboot.h',
        'include/abi-bits/resource.h',
        'include/abi-bits/riscv-hwprobe.h',
        'include/abi-bits/rlim_t.h',
        'include/abi-bits/seek-whence.h',
        'include/abi-bits/shm.h',
        'include/abi-bits/sigevent.h',
        'include/abi-bits/signal.h',
        'include/abi-bits/sigval.h',
        'include/abi-bits/socket.h',
        'include/abi-bits/socklen_t.h',
        'include/abi-bits/stat.h',
        'include/abi-bits/statfs.h',
        'include/abi-bits/statvfs.h',
        'include/abi-bits/statx.h',
        'include/abi-bits/suseconds_t.h',
        'include/abi-bits/termios.h',
        'include/abi-bits/time.h',
        'include/abi-bits/uid_t.h',
        'include/abi-bits/utmp-defines.h',
        'include/abi-bits/utmpx.h',
        'include/abi-bits/utsname.h',
        'include/abi-bits/vm-flags.h',
        'include/abi-bits/vt.h',
        'include/abi-bits/wait.h',
        'include/abi-bits/xattr.h',
        subdir: 'abi-bits',
        follow_symlinks: true
    )
endif

if not headers_only
    crt = custom_target('crt1',
        build_by_default: true,
        command: c_compiler.cmd_array() + ['-c', '-o', '@OUTPUT@', '@INPUT@'],
        input: 'crt1.S',
        output: 'crt1.o',
        install: true,
        install_dir: get_option('libdir')
    )
endif
