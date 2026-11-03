#!/bin/bash
# Build a minimal ext2 root for the x86-64 bring-up: /bin/sh is the freestanding
# smoke-test payload from usr/x86_64/init.c. Output: initrd.ext2 (or $1).
set -euo pipefail
OUT="${1:-initrd.ext2}"
ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT
mkdir -p "$ROOT"/{bin,dev,tmp,etc,root,lib,proc}
gcc -static -nostdlib -ffreestanding -fno-pic -no-pie -fno-stack-protector \
    -fno-asynchronous-unwind-tables -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 \
    -O1 -Wall -o "$ROOT/bin/smoke" usr/x86_64/init.c
# /bin/sh is what the kernel launches. Default: the libc-free smoke test (which
# is also a tiny shell). With AUTOEXIT unset and BusyBox built, /bin/sh becomes
# BusyBox ash instead; the smoke test stays available as /bin/smoke.
cp "$ROOT/bin/smoke" "$ROOT/bin/sh"
echo "extron x86-64" > "$ROOT/etc/hostname"
printf 'root:x:0:0:root:/root:/bin/sh\n' > "$ROOT/etc/passwd"
printf 'root:x:0:\n' > "$ROOT/etc/group"
# Optional mlibc userland (see docs/X86_64_UEFI_PORT.md): if a built sysroot and
# test programs exist, ship the runtime linker, libc and the programs.
UL="${EXTRON_USERLAND:-$(pwd)/build-x86_64/userland}"
if [ -f "$UL/sysroot/usr/lib/ld.so" ]; then
    cp "$UL/sysroot/usr/lib/ld.so" "$UL/sysroot/usr/lib/libc.so" "$ROOT/lib/"
    # Shared libraries for Python's modules (and anything else that links them):
    # ordinary system libraries in /lib, found by ld.so's default search path.
    EXTL="$(pwd)/build-x86_64/extlibs/prefix/lib"
    if [ -d "$EXTL" ]; then
        for f in "$EXTL"/*.so.*; do
            case "$f" in *.old) continue;; esac
            [ -e "$f" ] || continue
            cp -a "$f" "$ROOT/lib/"
            [ -L "$f" ] || strip --strip-unneeded "$ROOT/lib/$(basename "$f")" 2>/dev/null || true
        done
    fi
    for f in "$UL"/out/*; do [ -f "$f" ] && install -m 755 "$f" "$ROOT/bin/$(basename "$f")"; done
    # mlibc regression programs (usr/mlibc_tests) live on the data disk
    # (tools/build_x86_disk.sh), mounted at /mnt; /opt/tests resolves there.
    if [ -d "$UL/out/tests" ]; then
        ln -s /mnt/opt "$ROOT/opt"
        cp usr/hello.txt "$ROOT/hello.txt"
        cat > "$ROOT/bin/runtests" <<'RT'
#!/bin/sh
# Run every program in /opt/tests, report per-test status.
for t in /opt/tests/*.elf; do
    case "$t" in */mlibc_syscall_test.elf) continue;; esac   # interactive (reads the keyboard)
    echo "=== $t"
    "$t" || echo "[exit status $?]"
done
for t in /opt/tests/*.sh; do
    echo "=== $t"
    sh "$t" || echo "[exit status $?]"
done
# Python programs run only when the data disk carries CPython.
if [ -x /mnt/python/bin/python3 ]; then
    for t in /opt/tests/*.py; do
        echo "=== $t"
        /mnt/python/bin/python3 "$t" || echo "[exit status $?]"
    done
fi
echo "RUNTESTS DONE"
RT
        chmod 755 "$ROOT/bin/runtests"
    fi
    if [ -x "$ROOT/bin/busybox" ] && [ "${AUTOEXIT:-0}" != 1 ] && [ "${SMOKE_SHELL:-0}" != 1 ]; then
        rm -f "$ROOT/bin/sh"; ln -s busybox "$ROOT/bin/sh"
        # One symlink per applet (init, getty, login, sync, gzip, ... are all
        # exec'd by path, e.g. inittab's /bin/sync and tar's `gzip`).
        if [ -f "$UL/busybox.links" ]; then
            while IFS= read -r link; do
                [ -n "$link" ] || continue
                mkdir -p "$ROOT$(dirname "$link")"
                ln -sf /bin/busybox "$ROOT$link"
            done < "$UL/busybox.links"
        fi
        # python3 lives on the data disk (/mnt/python, see build_x86_python.sh).
        if [ -d build-x86_64/python/rootfs/python ]; then
            mkdir -p "$ROOT/usr/bin"
            ln -sf /mnt/python/bin/python3 "$ROOT/usr/bin/python3"
            ln -sf /mnt/python/bin/python3 "$ROOT/bin/python3"
        fi
        mkdir -p "$ROOT/sbin" "$ROOT/root" "$ROOT/home/user" "$ROOT/etc/init.d" "$ROOT/run" "$ROOT/var/log"
        chmod 700 "$ROOT/root"; chmod 1777 "$ROOT/tmp"

        # The system the kernel boots into: init -> getty -> login -> sh.
        cat > "$ROOT/etc/inittab" <<'IT'
::sysinit:/etc/init.d/rcS
::respawn:/sbin/getty -L 115200 console vt100
::ctrlaltdel:/sbin/reboot
::shutdown:/bin/sync
::restart:/sbin/init
IT
        cat > "$ROOT/etc/init.d/rcS" <<'RC'
#!/bin/sh
# Early system setup, run once by init before the login prompt appears.
hostname -F /etc/hostname 2>/dev/null
mkdir -p /tmp /run /var/log
chmod 1777 /tmp
RC
        chmod 755 "$ROOT/etc/init.d/rcS"
        printf 'Welcome to Extron.\n\n' > "$ROOT/etc/issue"
        printf '\nExtron OS - a hobby POSIX kernel. Type "poweroff" to shut down.\n\n' > "$ROOT/etc/motd"
        cat > "$ROOT/etc/profile" <<'PF'
export PATH=/sbin:/usr/sbin:/bin:/usr/bin:/opt/tests
export PAGER=less
alias ll='ls -l'
PF
        printf 'root:x:0:0:root:/root:/bin/sh\nuser:x:1000:1000:Extron user:/home/user:/bin/sh\n' > "$ROOT/etc/passwd"
        printf 'root:x:0:\nuser:x:1000:\n' > "$ROOT/etc/group"
        # root has no password (development system); `user` has the password "user".
        USERHASH=$(openssl passwd -1 user 2>/dev/null || echo '*')
        printf 'root::19000:0:99999:7:::\nuser:%s:19000:0:99999:7:::\n' "$USERHASH" > "$ROOT/etc/shadow"
        chmod 600 "$ROOT/etc/shadow"
        printf '/bin/sh\n' > "$ROOT/etc/shells"
        chown_user=1
    fi
fi
# AUTOEXIT=1 (used by `make check`): smoke test resets the VM when done instead of
# dropping into the interactive mini shell.
[ "${AUTOEXIT:-0}" = 1 ] && touch "$ROOT/etc/autoexit"
rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1M count=${IMG_MB:-32} status=none
if [ "${chown_user:-0}" = 1 ] && command -v fakeroot >/dev/null; then
    # Record real ownership (root everywhere, uid 1000 for the user's home)
    # without needing root on the build host.
    fakeroot sh -c "chown -R 0:0 '$ROOT' && chown 1000:1000 '$ROOT/home/user' && \
        mkfs.ext2 -q -E revision=0 -b 1024 -d '$ROOT' '$OUT'"
else
    mkfs.ext2 -q -E revision=0 -b 1024 -d "$ROOT" "$OUT"
fi
echo "built $OUT"
