#!/bin/bash
# Cross-build OpenSSH (portable) against the mlibc sysroot, OpenSSL and zlib from
# build_x86_libs.sh. Output tree (installed into the root disk by build_x86_rootfs.sh):
#     build-x86_64/openssh/rootfs/{usr/bin,usr/sbin,usr/libexec,etc/ssh}
#   tools/build_x86_openssh.sh        # REBUILD=1 to reconfigure from scratch
# The tarball checksum below is not an upstream-published one (OpenSSH only signs with PGP):
# it is the value three independent OpenBSD mirrors served for this release.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
UL="${EXTRON_USERLAND:-$REPO/build-x86_64/userland}"
B="$REPO/build-x86_64/openssh"
PREFIX="$REPO/build-x86_64/extlibs/prefix"
VER=10.6p2
SHA=9ae44872090cf2f06b3cf9ff147e7f16f309671340763ed2f99248398ae29d5b
TAR="$REPO/build-x86_64/extlibs/dl/openssh-$VER.tar.gz"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
export EXTRON_SYSROOT="$UL/sysroot"
export CC="$REPO/tools/x86_64-extron-gcc"
[ -f "$PREFIX/lib/libcrypto.so.3" ] || { echo "build openssl first (tools/build_x86_libs.sh openssl)" >&2; exit 1; }

mkdir -p "$B"
[ -f "$TAR" ] || curl -fsSL "https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/openssh-$VER.tar.gz" -o "$TAR"
echo "$SHA  $TAR" | sha256sum -c - >/dev/null
SRC="$B/openssh-$VER"
if [ ! -d "$SRC" ] || [ "${REBUILD:-0}" = 1 ]; then
    rm -rf "$SRC"; tar -xzf "$TAR" -C "$B"
fi
cd "$SRC"
# mlibc has no crypt(): password authentication cannot work, so xcrypt() always fails and the
# shipped sshd_config is key-only. (Applied idempotently to the unpacked source.)
python3 - <<'PY'
import re
f = "openbsd-compat/xcrypt.c"
s = open(f).read()
s = s.replace("# else\n\tcrypted = crypt(password, salt);", "# else\n\t(void)password; crypted = NULL;   /* no crypt() on Extron */")
open(f, "w").write(s)
# The kernel has no per-process root, so chroot() is ENOSYS: the pre-authentication child is
# still dropped to the unprivileged sshd user (setgroups/setuid), but it is not jailed in
# /var/empty. Skipping the call is a deliberate, documented loss of defence in depth.
for f in ("sshd-auth.c", "sshd-session.c"):
    t = open(f).read()
    t = t.replace("if (chroot(_PATH_PRIVSEP_CHROOT_DIR) == -1)", "if (0 /* Extron: no chroot() */ && chroot(_PATH_PRIVSEP_CHROOT_DIR) == -1)")
    open(f, "w").write(t)
# getrrsetbyname() (DNS host-key lookup) needs arpa/nameser.h and the res_* API, which mlibc
# lacks: keep the declarations, answer every query with ERRSET_FAIL.
h = "openbsd-compat/getrrsetbyname.h"
t = open(h).read()
t = t.replace("#include <arpa/nameser.h>\n", "").replace("#include <resolv.h>\n", "")
open(h, "w").write(t)
open("openbsd-compat/getrrsetbyname.c", "w").write(
    '#include "includes.h"\n#include <stdlib.h>\n#include "getrrsetbyname.h"\n'
    '#ifndef HAVE_GETRRSETBYNAME\n'
    'int getrrsetbyname(const char *h, unsigned int c, unsigned int t, unsigned int f, struct rrsetinfo **r)\n'
    '{ (void)h; (void)c; (void)t; (void)f; *r = NULL; return ERRSET_FAIL; }\n'
    'void freerrset(struct rrsetinfo *r) { (void)r; }\n#endif\n')
PY
if [ ! -f Makefile ] || [ "${REBUILD:-0}" = 1 ]; then
    ./configure --build=x86_64-pc-linux-gnu --host=x86_64-unknown-linux-gnu \
        --prefix=/usr --sysconfdir=/etc/ssh --libexecdir=/usr/libexec \
        --with-ssl-dir="$PREFIX" --with-zlib="$PREFIX" --without-pam --with-sandbox=no --disable-strip \
        --with-privsep-path=/var/empty --with-pid-dir=/run --disable-lastlog --disable-utmp \
        --disable-wtmp --disable-utmpx --disable-wtmpx \
        ac_cv_header_utmp_h=no ac_cv_header_utmpx_h=no \
        CFLAGS="-O2" LDFLAGS="-L$PREFIX/lib -Wl,-rpath-link,$PREFIX/lib" CPPFLAGS="-I$PREFIX/include -DI_PUSH=0x5302" \
        >"$B/configure.log" 2>&1 || { tail -30 "$B/configure.log"; exit 1; }
fi
# configure treats the (pretend) linux host as having btmp/utmp login records; Extron has none.
sed -i 's|^#define USE_BTMP 1|/* #undef USE_BTMP */|' config.h
# (-DI_PUSH: openpty's STREAMS fallback names it; the ioctls just fail harmlessly here.)
# configure skips the /dev/ptmx probe when cross-compiling; the kernel has /dev/ptmx (mlibc: posix_openpt).
sed -i 's|^/\* #undef HAVE_DEV_PTMX \*/|#define HAVE_DEV_PTMX 1|' config.h
# No pre-auth sandbox (--with-sandbox=no): there is no seccomp, and the rlimit sandbox would need
# setrlimit() of FSIZE/NPROC (ENOTSUP: the kernel only enforces RLIMIT_NOFILE) and a select()/
# poll() that works at NOFILE=0, which cannot be probed while cross-compiling. The pre-auth
# child still runs as the unprivileged sshd user.
sed -i 's|^#define HAVE_RLIMIT_NPROC.*|/* #undef HAVE_RLIMIT_NPROC */|' config.h
make -j"$JOBS" >"$B/make.log" 2>&1 || { grep -E "error|Error" "$B/make.log" | head -20; exit 1; }
rm -rf "$B/rootfs"
make install-nokeys DESTDIR="$B/rootfs" STRIP_OPT= >"$B/install.log" 2>&1 || { tail -20 "$B/install.log"; exit 1; }
echo "openssh $VER installed into $B/rootfs"
