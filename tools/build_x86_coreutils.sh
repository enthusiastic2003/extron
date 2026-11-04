#!/bin/bash
# Cross-build GNU coreutils (one multi-call binary + applet symlinks) against the
# mlibc sysroot with the compiler wrapper. Output:
#     build-x86_64/coreutils/rootfs/usr/{bin,libexec}   (staged for the initrd)
#   tools/build_x86_coreutils.sh        # REBUILD=1 to reconfigure from scratch
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
UL="${EXTRON_USERLAND:-$REPO/build-x86_64/userland}"
B="$REPO/build-x86_64/coreutils"
VER=9.12
SHA=a480198559733e9b3da999e90543ac6f888a2caa544d8d664c5a1f17e528e210
TAR="$REPO/build-x86_64/extlibs/dl/coreutils-$VER.tar.xz"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
export EXTRON_SYSROOT="$UL/sysroot"
export CC="$REPO/tools/x86_64-extron-gcc"
[ -f "$EXTRON_SYSROOT/usr/lib/libc.so" ] || { echo "build the userland first" >&2; exit 1; }

mkdir -p "$B" "$(dirname "$TAR")"
[ -f "$TAR" ] || curl -fsSL "https://ftp.gnu.org/gnu/coreutils/coreutils-$VER.tar.xz" -o "$TAR"
echo "$SHA  $TAR" | sha256sum -c - >/dev/null
SRC="$B/coreutils-$VER"
if [ ! -d "$SRC" ] || [ "${REBUILD:-0}" = 1 ]; then
    rm -rf "$SRC" "$B/b"; tar -xf "$TAR" -C "$B"
    # gnulib's getlocalename_l has no branch for mlibc (which keeps no locale
    # names): report the C locale.
    python3 - "$SRC/lib/getlocalename_l-unsafe.c" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "#elif defined __ANDROID__\n      /* Android API level >= 21 */"
new = ("#elif defined __extron__\n"
       "      return (struct string_with_storage) { \"C\", STORAGE_INDEFINITE };\n" + old)
assert old in s
open(p, "w").write(s.replace(old, new, 1))
PY
fi

if [ ! -f "$B/b/Makefile" ]; then
    mkdir -p "$B/b"; cd "$B/b"
    "$SRC/configure" --build=x86_64-pc-linux-gnu --host=x86_64-unknown-linux-gnu \
        --prefix=/usr --disable-nls --disable-acl --disable-xattr --without-selinux \
        --without-libgmp --disable-libcap --enable-single-binary=symlinks \
        --disable-dependency-tracking CFLAGS="-O2" >"$B/configure.log" 2>&1 \
        || { tail -30 "$B/configure.log"; exit 1; }
fi
cd "$B/b"
make SUBDIRS=. -j"$JOBS" >"$B/make.log" 2>&1 || { grep -E "error|Error" "$B/make.log" | head -20; exit 1; }
rm -rf "$B/rootfs"
make SUBDIRS=. install DESTDIR="$B/rootfs" >"$B/install.log" 2>&1 || { tail -20 "$B/install.log"; exit 1; }
strip --strip-unneeded "$B/rootfs/usr/bin/coreutils" 2>/dev/null || true
echo "coreutils $VER: $(ls "$B/rootfs/usr/bin" | wc -l) tools, binary $(stat -c %s "$B/rootfs/usr/bin/coreutils") bytes"
