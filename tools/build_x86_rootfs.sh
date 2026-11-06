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

# Development files for those libraries (headers, libfoo.so / libfoo.a, pkg-config, *-config),
# so source built on the OS can link against them. Build-host prefixes are rewritten to /usr.
EXTP="$REPO/build-x86_64/extlibs/prefix"
if [ -d "$EXTP/include" ]; then
    cp -a "$EXTP"/include/. "$ROOT/usr/include/"
    for f in "$EXTP"/lib/*.a "$EXTP"/lib/*.so; do
        [ -e "$f" ] || [ -L "$f" ] || continue
        cp -a "$f" "$ROOT/usr/lib/"
    done
    # Non-wide names resolve to the wide library (ncurses is built wide only).
    for n in ncurses panel menu form; do
        [ -e "$EXTP/lib/lib${n}w.so" ] && ln -sf lib${n}w.so "$ROOT/usr/lib/lib$n.so"
    done
    mkdir -p "$ROOT/usr/lib/pkgconfig"
    for f in "$EXTP"/lib/pkgconfig/*.pc; do
        [ -e "$f" ] && sed "s|$EXTP|/usr|g" "$f" > "$ROOT/usr/lib/pkgconfig/$(basename "$f")"
    done
    for f in "$EXTP"/bin/*-config; do
        [ -e "$f" ] && sed "s|$EXTP|/usr|g" "$f" > "$ROOT/usr/bin/$(basename "$f")" \
            && chmod 755 "$ROOT/usr/bin/$(basename "$f")"
    done
fi

# OpenSSL: the command, its config, and the CA bundle (copied from the build host's
# Mozilla-derived bundle; /etc/ssl is the --openssldir the library was built with).
OSSL="$REPO/build-x86_64/extlibs/work/openssl"
if [ -x "$OSSL/apps/openssl" ]; then
    install -m 755 "$OSSL/apps/openssl" "$ROOT/usr/bin/openssl"
    strip --strip-unneeded "$ROOT/usr/bin/openssl" 2>/dev/null || true
    mkdir -p "$ROOT/etc/ssl/certs" "$ROOT/etc/ssl/private"
    install -m 644 "$OSSL/apps/openssl.cnf" "$ROOT/etc/ssl/openssl.cnf"
    chmod 700 "$ROOT/etc/ssl/private"
    if [ -f /etc/ssl/certs/ca-certificates.crt ]; then
        install -m 644 /etc/ssl/certs/ca-certificates.crt "$ROOT/etc/ssl/certs/ca-certificates.crt"
        ln -sf certs/ca-certificates.crt "$ROOT/etc/ssl/cert.pem"
    fi
fi

# OpenSSH (tools/build_x86_openssh.sh): client tools, sshd and its helpers. Password
# authentication is unavailable (mlibc has no crypt()), so sshd_config is key-only.
OSSH="$REPO/build-x86_64/openssh/rootfs"
if [ -x "$OSSH/usr/sbin/sshd" ]; then
    for d in usr/bin usr/sbin usr/libexec; do
        mkdir -p "$ROOT/$d"
        for f in "$OSSH/$d"/*; do
            install -m 755 "$f" "$ROOT/$d/$(basename "$f")"
            strip --strip-unneeded "$ROOT/$d/$(basename "$f")" 2>/dev/null || true
        done
    done
    mkdir -p "$ROOT/etc/ssh" "$ROOT/var/empty" "$ROOT/run"
    chmod 755 "$ROOT/var/empty"
    install -m 644 "$OSSH/etc/ssh/moduli" "$OSSH/etc/ssh/ssh_config" "$ROOT/etc/ssh/"
    install -m 644 usr/x86_64/config/sshd_config "$ROOT/etc/ssh/sshd_config"
    install -m 755 usr/x86_64/config/start-sshd "$ROOT/usr/sbin/start-sshd"
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

# ---- GNU make (tools/build_x86_make.sh) ------------------------------------------
[ -x build-x86_64/make/rootfs/usr/bin/make ] && \
    install -m 755 build-x86_64/make/rootfs/usr/bin/make "$ROOT/usr/bin/make"

# ---- C compiler: tcc, the C headers and link-time files (tools/build_x86_tcc.sh) ---------
if [ -x build-x86_64/tcc/rootfs/usr/bin/tcc ]; then
    cp -a build-x86_64/tcc/rootfs/usr/. "$ROOT/usr/"
    SR="$UL/sysroot/usr"
    cp -a "$SR/include" "$ROOT/usr/"                       # mlibc and Linux UAPI headers
    for f in "$SR"/lib/*; do                               # crt1.o, libc.a, libm.so, ...
        [ -e "$ROOT/usr/lib/$(basename "$f")" ] || cp -a "$f" "$ROOT/usr/lib/"
    done
    # Empty crti.o/crtn.o: tcc always links them, mlibc's crt1.o does all the work.
    for o in crti crtn; do echo "" | gcc -x c -c -o "$ROOT/usr/lib/$o.o" -; done
    ln -sf tcc "$ROOT/usr/bin/cc"
    printf '#!/bin/sh\nexec tcc -ar "$@"\n' > "$ROOT/usr/bin/ar"
    printf '#!/bin/sh\n# tcc -ar already writes the symbol index\nexit 0\n' > "$ROOT/usr/bin/ranlib"
    # ld: configure scripts (libtool) insist on finding one. tcc is the linker; this wrapper
    # answers the version probes and forwards real link lines (dropping -m <emulation>).
    cat > "$ROOT/usr/bin/ld" <<'LDEOF'
#!/bin/sh
# Minimal ld on top of tcc (no GNU binutils on Extron).
for a in "$@"; do
    case $a in
        -v|-V|--version) echo "ld (tcc linker wrapper) 0.9.28"; exit 0;;
    esac
done
args=
while [ $# -gt 0 ]; do
    case $1 in
        -m) shift;;                       # -m elf_x86_64
        -m*) ;;
        *) args="$args '$1'";;
    esac
    shift
done
eval exec tcc -nostdlib "$args"
LDEOF
    chmod 755 "$ROOT/usr/bin/ar" "$ROOT/usr/bin/ranlib" "$ROOT/usr/bin/ld"
fi

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
    # Data for the on-OS toolchain test: an ABI program with the output gcc+glibc gives, the
    # socket test source, and zlib's tarball (if it was downloaded for the library build).
    mkdir -p "$ROOT/opt/tests/data"
    cp usr/x86_64/tests/data/* "$ROOT/opt/tests/data/"
    cp usr/mlibc_tests/mlibc_net_test.c "$ROOT/opt/tests/data/net_test.c"
    [ -f build-x86_64/extlibs/dl/zlib-1.3.1.tar.gz ] && cp build-x86_64/extlibs/dl/zlib-1.3.1.tar.gz "$ROOT/opt/tests/data/"
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
# Autotools: config.guess/config.sub that recognise Extron (tarballs ship older copies);
# `update-config [dir]` copies them over a source tree's.
mkdir -p "$ROOT/usr/share/misc"
install -m 644 usr/x86_64/config/config.guess usr/x86_64/config/config.sub "$ROOT/usr/share/misc/"
install -m 755 usr/x86_64/config/update-config "$ROOT/usr/bin/update-config"

# Name resolution (mlibc's resolver): /etc/resolv.conf is the kernel's live view of what
# DHCP learned (/proc/net/resolv.conf); the service and protocol tables are static.
printf '127.0.0.1\tlocalhost extron\n' > "$ROOT/etc/hosts"
ln -sf /proc/net/resolv.conf "$ROOT/etc/resolv.conf"
cat > "$ROOT/etc/services" <<'SV'
echo		7/tcp
echo		7/udp
ftp-data	20/tcp
ftp		21/tcp
ssh		22/tcp
telnet		23/tcp
smtp		25/tcp
domain		53/tcp
domain		53/udp
bootps		67/udp
bootpc		68/udp
tftp		69/udp
http		80/tcp
pop3		110/tcp
ntp		123/udp
imap		143/tcp
https		443/tcp
SV
cat > "$ROOT/etc/protocols" <<'PR'
ip	0	IP
icmp	1	ICMP
tcp	6	TCP
udp	17	UDP
PR
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
printf 'root:x:0:0:root:/root:%s\nuser:x:1000:1000:Extron user:/home/user:%s\nsshd:x:74:74:sshd privilege separation:/var/empty:/bin/false\n' "$LOGIN_SH" "$LOGIN_SH" > "$ROOT/etc/passwd"
printf 'root:x:0:\nuser:x:1000:\nsshd:x:74:\n' > "$ROOT/etc/group"
# root has no password (development system); `user` has the password "user".
USERHASH=$(openssl passwd -1 user 2>/dev/null || echo '*')
printf 'root::19000:0:99999:7:::\nuser:%s:19000:0:99999:7:::\nsshd:*:19000:0:99999:7:::\n' "$USERHASH" > "$ROOT/etc/shadow"
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
