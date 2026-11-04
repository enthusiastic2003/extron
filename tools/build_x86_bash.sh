#!/bin/bash
# Cross-build GNU bash (bundled readline, ncursesw from build_x86_libs.sh) against
# the mlibc sysroot. Output: build-x86_64/bash/rootfs/usr/bin/bash (staged for the initrd).
#   tools/build_x86_bash.sh        # REBUILD=1 to reconfigure from scratch
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
UL="${EXTRON_USERLAND:-$REPO/build-x86_64/userland}"
EXT="$REPO/build-x86_64/extlibs"; P="$EXT/prefix"
B="$REPO/build-x86_64/bash"
VER=5.3
SHA=0d5cd86965f869a26cf64f4b71be7b96f90a3ba8b3d74e27e8e9d9d5550f31ba
TAR="$EXT/dl/bash-$VER.tar.gz"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
export EXTRON_SYSROOT="$UL/sysroot"
export CC="$REPO/tools/x86_64-extron-gcc"
[ -f "$EXTRON_SYSROOT/usr/lib/libc.so" ] || { echo "build the userland first" >&2; exit 1; }
[ -f "$EXT/.built-ncurses" ] || tools/build_x86_libs.sh ncurses

mkdir -p "$B" "$(dirname "$TAR")"
[ -f "$TAR" ] || curl -fsSL "https://ftp.gnu.org/gnu/bash/bash-$VER.tar.gz" -o "$TAR"
echo "$SHA  $TAR" | sha256sum -c - >/dev/null
SRC="$B/bash-$VER"
if [ ! -d "$SRC" ] || [ "${REBUILD:-0}" = 1 ]; then
    rm -rf "$SRC" "$B/b"; tar -xf "$TAR" -C "$B"
fi
if [ ! -f "$B/b/Makefile" ]; then
    mkdir -p "$B/b"; cd "$B/b"
    # Cross-compiling: the helper programs that generate bash's sources run on
    # the build machine (CC_FOR_BUILD), and the run-time probes are answered here.
    "$SRC/configure" --build=x86_64-pc-linux-gnu --host=x86_64-unknown-linux-gnu \
        --prefix=/usr --without-bash-malloc --enable-readline --disable-nls --with-curses \
        CC_FOR_BUILD=gcc bash_cv_getcwd_malloc=yes bash_cv_job_control_missing=present \
        bash_cv_sys_named_pipes=present bash_cv_func_sigsetjmp=present \
        bash_cv_wcontinued_broken=no bash_cv_termcap_lib=libncursesw bash_cv_dev_fd=absent bash_cv_dev_stdin=absent \
        CFLAGS="-O2 -I$P/include -I$P/include/ncursesw" LDFLAGS="-L$P/lib" \
        >"$B/configure.log" 2>&1 || { tail -30 "$B/configure.log"; exit 1; }
fi
cd "$B/b"
make -j"$JOBS" >"$B/make.log" 2>&1 || { grep -E "error|Error" "$B/make.log" | head -20; exit 1; }
rm -rf "$B/rootfs"; mkdir -p "$B/rootfs/usr/bin"
install -m 755 bash "$B/rootfs/usr/bin/bash"
strip --strip-unneeded "$B/rootfs/usr/bin/bash" 2>/dev/null || true
echo "bash $VER: $(stat -c %s "$B/rootfs/usr/bin/bash") bytes"
