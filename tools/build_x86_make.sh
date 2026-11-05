#!/bin/bash
# Cross-build GNU make against the mlibc sysroot with the compiler wrapper. Output:
#     build-x86_64/make/rootfs/usr/bin/make   (installed into the root disk by build_x86_rootfs.sh)
#   tools/build_x86_make.sh        # REBUILD=1 to reconfigure from scratch
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
UL="${EXTRON_USERLAND:-$REPO/build-x86_64/userland}"
B="$REPO/build-x86_64/make"
VER=4.4.1
SHA=dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3
TAR="$REPO/build-x86_64/extlibs/dl/make-$VER.tar.gz"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
export EXTRON_SYSROOT="$UL/sysroot"
export CC="$REPO/tools/x86_64-extron-gcc"
[ -f "$EXTRON_SYSROOT/usr/lib/libc.so" ] || { echo "build the userland first" >&2; exit 1; }

mkdir -p "$B" "$(dirname "$TAR")"
[ -f "$TAR" ] || curl -fsSL "https://ftp.gnu.org/gnu/make/make-$VER.tar.gz" -o "$TAR"
echo "$SHA  $TAR" | sha256sum -c - >/dev/null
SRC="$B/make-$VER"
if [ ! -d "$SRC" ] || [ "${REBUILD:-0}" = 1 ]; then
    rm -rf "$SRC" "$B/b"; tar -xzf "$TAR" -C "$B"
fi
if [ ! -f "$B/b/Makefile" ]; then
    mkdir -p "$B/b"; cd "$B/b"
    "$SRC/configure" --build=x86_64-pc-linux-gnu --host=x86_64-unknown-linux-gnu \
        --prefix=/usr --disable-nls --without-guile --disable-load --disable-dependency-tracking \
        CFLAGS="-O2 -std=gnu17" >"$B/configure.log" 2>&1 || { tail -30 "$B/configure.log"; exit 1; }
fi
cd "$B/b"
make -j"$JOBS" >"$B/make.log" 2>&1 || { grep -E "error|Error" "$B/make.log" | head -20; exit 1; }
rm -rf "$B/rootfs"; mkdir -p "$B/rootfs/usr/bin"
install -m 755 make "$B/rootfs/usr/bin/make"
strip --strip-unneeded "$B/rootfs/usr/bin/make" 2>/dev/null || true
echo "make $VER: $(stat -c %s "$B/rootfs/usr/bin/make") bytes"
