#!/bin/bash
# Build the root filesystem image (ext2) the x86-64 kernel boots from
# (root=/dev/vda). A normal Linux-style tree: merged /usr (/bin, /sbin, /lib are
# symlinks into it), shared libraries in /usr/lib, GNU coreutils and bash in
# /usr/bin with BusyBox filling in the applets GNU does not provide, Python under
# /usr, the regression tests in /opt/tests.
#
#   tools/build_x86_rootfs.sh [OUT [SIZE_MB]]      default build-x86_64/rootfs.img, 512 MB
# KEEP=1: carry /root, /home and /mnt over from an existing OUT (refresh the system,
# keep the user's files). The image is sparse, so its size on the host stays small.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"
OUT="${1:-build-x86_64/rootfs.img}"
MB="${2:-${ROOTFS_MB:-512}}"
UL="${EXTRON_USERLAND:-$REPO/build-x86_64/userland}"
EXTL="$REPO/build-x86_64/extlibs/prefix/lib"
[ -f "$UL/sysroot/usr/lib/ld.so" ] || { echo "build the userland first (make userland)" >&2; exit 1; }

ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT

# ---- skeleton --------------------------------------------------------------
mkdir -p "$ROOT"/usr/{bin,sbin,lib,libexec} "$ROOT"/{dev,proc,sys,mnt,tmp,run,root,etc/init.d} \
         "$ROOT"/var/{log,tmp,run} "$ROOT"/home/user "$ROOT"/opt/tests
ln -s usr/bin "$ROOT/bin"; ln -s usr/sbin "$ROOT/sbin"; ln -s usr/lib "$ROOT/lib"
chmod 700 "$ROOT/root"; chmod 1777 "$ROOT/tmp" "$ROOT/var/tmp"

# ---- libc, dynamic linker, shared libraries --------------------------------
cp "$UL/sysroot/usr/lib/ld.so" "$UL/sysroot/usr/lib/libc.so" "$ROOT/usr/lib/"
if [ -d "$EXTL" ]; then
    for f in "$EXTL"/*.so.*; do
        case "$f" in *.old) continue;; esac
        [ -e "$f" ] || continue
        cp -a "$f" "$ROOT/usr/lib/"
        [ -L "$f" ] || strip --strip-unneeded "$ROOT/usr/lib/$(basename "$f")" 2>/dev/null || true
    done
fi

# ---- BusyBox: init, getty, login and every applet GNU does not replace ------
for f in "$UL"/out/*; do
    [ -f "$f" ] || continue
    case "$(basename "$f")" in initramfs-init) continue;; esac     # only for the initramfs
    install -m 755 "$f" "$ROOT/usr/bin/$(basename "$f")"
done
if [ -f "$UL/busybox.links" ]; then
    while IFS= read -r link; do
        [ -n "$link" ] || continue
        mkdir -p "$ROOT$(dirname "$link")"
        ln -sf /bin/busybox "$ROOT$link"
    done < "$UL/busybox.links"
fi
ln -sf busybox "$ROOT/usr/bin/sh"

# ---- GNU coreutils: its names win over BusyBox's -----------------------------
CU=build-x86_64/coreutils/rootfs/usr
if [ -x "$CU/bin/coreutils" ]; then
    install -m 755 "$CU/bin/coreutils" "$ROOT/usr/bin/coreutils"
    for l in "$CU"/bin/*; do
        [ -L "$l" ] && ln -sfn coreutils "$ROOT/usr/bin/$(basename "$l")"
    done
    [ -d "$CU/libexec/coreutils" ] && cp -a "$CU/libexec/coreutils" "$ROOT/usr/libexec/"
fi

# ---- bash --------------------------------------------------------------------
HAVE_BASH=0
if [ -x build-x86_64/bash/rootfs/usr/bin/bash ]; then
    install -m 755 build-x86_64/bash/rootfs/usr/bin/bash "$ROOT/usr/bin/bash"; HAVE_BASH=1
fi

# ---- Python ------------------------------------------------------------------
[ -d build-x86_64/python/rootfs/usr ] && cp -a build-x86_64/python/rootfs/usr/. "$ROOT/usr/"

# ---- DOOM (tools/build_x86_doom.sh) ---------------------------------------------
if [ -x build-x86_64/doom/doom ]; then
    install -m 755 build-x86_64/doom/doom "$ROOT/usr/bin/doom"
    mkdir -p "$ROOT/usr/share/games/doom"
    cp build-x86_64/extlibs/dl/doom1.wad "$ROOT/usr/share/games/doom/doom1.wad"
fi

# ---- regression tests ---------------------------------------------------------
if [ -d "$UL/out/tests" ]; then
    cp "$UL"/out/tests/* "$ROOT/opt/tests/"
    cp usr/hello.txt "$ROOT/hello.txt"; cp usr/hello.txt "$ROOT/opt/tests/hello.txt"
    for s in usr/x86_64/tests/*.sh usr/x86_64/tests/*.bash usr/x86_64/tests/*.py; do
        [ -f "$s" ] && install -m 755 "$s" "$ROOT/opt/tests/"
    done
    cat > "$ROOT/usr/bin/runtests" <<'RT'
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
# bash and Python programs run only when those are installed.
if [ -x /bin/bash ]; then
    for t in /opt/tests/*.bash; do
        echo "=== $t"
        /bin/bash "$t" || echo "[exit status $?]"
    done
fi
if [ -x /usr/bin/python3 ]; then
    for t in /opt/tests/*.py; do
        echo "=== $t"
        /usr/bin/python3 "$t" || echo "[exit status $?]"
    done
fi
echo "RUNTESTS DONE"
RT
    chmod 755 "$ROOT/usr/bin/runtests"
fi

# ---- configuration ------------------------------------------------------------
echo "extron" > "$ROOT/etc/hostname"
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
mkdir -p /tmp /run /var/log /var/tmp
chmod 1777 /tmp /var/tmp
RC
chmod 755 "$ROOT/etc/init.d/rcS"
printf 'Welcome to Extron.\n\n' > "$ROOT/etc/issue"
printf '\nExtron OS - a hobby POSIX kernel. Type "poweroff" to shut down.\n\n' > "$ROOT/etc/motd"
cat > "$ROOT/etc/profile" <<'PF'
export PATH=/usr/local/bin:/usr/bin:/usr/sbin:/bin:/sbin:/opt/tests
export PAGER=less
# login(1) leaves the identity variables for the shell to derive; export them
# so children (and bash -c) see them.
: ${USER:=$(id -un 2>/dev/null)}
export HOME USER LOGNAME=${LOGNAME:-$USER}
alias ll='ls -l'
if [ -n "$BASH_VERSION" ]; then
    export SHELL=/bin/bash INPUTRC=/etc/inputrc
    PS1='\w \$ '      # same shape as BusyBox's ("/ # "), which the test harnesses wait for
    HISTFILE=$HOME/.bash_history
    [ -r /etc/bash.bashrc ] && . /etc/bash.bashrc
fi
PF
printf '/bin/sh\n/bin/ash\n/bin/bash\n' > "$ROOT/etc/shells"
printf 'set enable-bracketed-paste off\n' > "$ROOT/etc/inputrc"
printf '/dev/vda / ext2 rw 0 1\nproc /proc proc defaults 0 0\ndevtmpfs /dev devtmpfs defaults 0 0\n' > "$ROOT/etc/fstab"
ln -sf /proc/mounts "$ROOT/etc/mtab"
LOGIN_SH=/bin/sh; [ "$HAVE_BASH" = 1 ] && LOGIN_SH=/bin/bash
printf 'root:x:0:0:root:/root:%s\nuser:x:1000:1000:Extron user:/home/user:%s\n' "$LOGIN_SH" "$LOGIN_SH" > "$ROOT/etc/passwd"
printf 'root:x:0:\nuser:x:1000:\n' > "$ROOT/etc/group"
# root has no password (development system); `user` has the password "user".
USERHASH=$(openssl passwd -1 user 2>/dev/null || echo '*')
printf 'root::19000:0:99999:7:::\nuser:%s:19000:0:99999:7:::\n' "$USERHASH" > "$ROOT/etc/shadow"
chmod 600 "$ROOT/etc/shadow"

# ---- keep the user's files across a rebuild ------------------------------------
if [ "${KEEP:-0}" = 1 ] && [ -f "$OUT" ]; then
    KEEPDIR="$(mktemp -d)"
    for d in root home mnt; do
        debugfs -R "rdump /$d $KEEPDIR" "$OUT" >/dev/null 2>&1 || true
        [ -d "$KEEPDIR/$d" ] && { rm -rf "${ROOT:?}/$d"; cp -a "$KEEPDIR/$d" "$ROOT/$d"; }
    done
    rm -rf "$KEEPDIR"
fi

# ---- image ------------------------------------------------------------------------
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
truncate -s "${MB}M" "$OUT"
MKFS="mke2fs -q -t ext2 -E revision=0 -b 1024 -d '$ROOT' '$OUT'"
if command -v fakeroot >/dev/null; then
    # Real ownership (root everywhere, uid 1000 for the user's home) without being root.
    fakeroot sh -c "chown -R 0:0 '$ROOT' && chown -R 1000:1000 '$ROOT/home/user' && $MKFS"
else
    sh -c "$MKFS"
fi
echo "built $OUT ($MB MB sparse, $(du -sh --apparent-size "$OUT" | cut -f1) apparent, $(du -sh "$OUT" | cut -f1) used)"
