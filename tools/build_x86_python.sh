#!/bin/bash
# Cross-build CPython for Extron (x86-64, mlibc) with the host GCC and the
# userland sysroot from tools/build_x86_userland.sh. Output:
#     build-x86_64/python/rootfs/usr/   merged into the root filesystem by
# tools/build_x86_rootfs.sh (interpreter /usr/bin/python3, standard library
# /usr/lib/python3.14, extension modules in /usr/lib/python3.14/lib-dynload).
#
# Needs: the userland sysroot, curl, and a host python3 of the SAME minor version
# (cross builds run the build python to freeze modules and compile bytecode).
# The C libraries behind the optional modules (zlib, bzip2, lzma, zstd, ctypes/libffi,
# curses, readline, sqlite3) come from tools/build_x86_libs.sh (run automatically if
# they have not been built). openssl and libuuid are not available, so _ssl, _hashlib
# and _uuid stay absent; every other standard module is built.
#
# Overrides: PY_VERSION PY_SHA256 JOBS REBUILD=1
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
UL="${EXTRON_USERLAND:-$REPO/build-x86_64/userland}"
PYDIR="$REPO/build-x86_64/python"
PY_VERSION="${PY_VERSION:-3.14.4}"
PY_SHA256="${PY_SHA256:-b4c059d5895f030e7df9663894ce3732bfa1b32cd3ab2883980266a45ce3cb3b}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
MM="${PY_VERSION%.*}"                                  # 3.14
export EXTRON_SYSROOT="$UL/sysroot"
CC="$REPO/tools/x86_64-extron-gcc"
BUILD_PY="${BUILD_PYTHON:-$(command -v python3)}"

say() { printf '\n== %s\n' "$*"; }
[ -f "$EXTRON_SYSROOT/usr/lib/libc.so" ] || { echo "build the userland first: make -f Makefile.x86_64 userland" >&2; exit 1; }
HOSTMM="$("$BUILD_PY" -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
[ "$HOSTMM" = "$MM" ] || { echo "host python $HOSTMM does not match $MM (set PY_VERSION or BUILD_PYTHON)" >&2; exit 1; }

EXT="$REPO/build-x86_64/extlibs/prefix"
[ -f "$REPO/build-x86_64/extlibs/.built-sqlite" ] || "$REPO/tools/build_x86_libs.sh"

mkdir -p "$PYDIR"
TARBALL="$PYDIR/Python-$PY_VERSION.tgz"
if [ ! -f "$TARBALL" ]; then
    say "downloading Python $PY_VERSION"
    curl -fsSL "https://www.python.org/ftp/python/$PY_VERSION/Python-$PY_VERSION.tgz" -o "$TARBALL"
fi
echo "$PY_SHA256  $TARBALL" | sha256sum -c - >/dev/null

if [ "${REBUILD:-0}" = 1 ] || [ ! -x "$PYDIR/build/python" ]; then
    say "configuring Python $PY_VERSION"
    rm -rf "$PYDIR/src" "$PYDIR/build"
    mkdir -p "$PYDIR/src" "$PYDIR/build"
    tar xzf "$TARBALL" --strip-components=1 -C "$PYDIR/src"
    cat > "$PYDIR/config.site" <<'SITE'
# Answers configure cannot get by running programs (cross build for Extron).
ac_cv_file__dev_ptmx=yes
ac_cv_file__dev_ptc=no
ac_cv_buggy_getaddrinfo=no
ac_cv_have_long_long_format=yes
ac_cv_working_tzset=yes
# Extron's kernel does not enable AVX/XSAVE state, but CPython's BLAKE2 picks its
# 256-bit path from the CPUID feature bit alone (it never checks OSXSAVE/XGETBV),
# which raises #UD. SSE (what the kernel does enable) stays on.
ax_cv_check_cflags__Werror__mavx2=no
SITE
    # No pkg-config here: tell configure where each library is.
    (cd "$PYDIR/build" &&
     CONFIG_SITE="$PYDIR/config.site" CC="$CC" CFLAGS="-O2" \
     CPPFLAGS="-I$EXT/include" LDFLAGS="-L$EXT/lib" \
     ZLIB_CFLAGS="-I$EXT/include" ZLIB_LIBS="-lz" \
     BZIP2_CFLAGS="-I$EXT/include" BZIP2_LIBS="-lbz2" \
     LIBLZMA_CFLAGS="-I$EXT/include" LIBLZMA_LIBS="-llzma" \
     LIBZSTD_CFLAGS="-I$EXT/include" LIBZSTD_LIBS="-lzstd" \
     LIBFFI_CFLAGS="-I$EXT/include" LIBFFI_LIBS="-lffi" \
     CURSES_CFLAGS="-I$EXT/include" CURSES_LIBS="-lncursesw" \
     PANEL_CFLAGS="-I$EXT/include" PANEL_LIBS="-lpanelw -lncursesw" \
     LIBSQLITE3_CFLAGS="-I$EXT/include" LIBSQLITE3_LIBS="-lsqlite3" \
     LIBREADLINE_CFLAGS="-I$EXT/include" LIBREADLINE_LIBS="-lreadline -lncursesw" \
        ../src/configure --build=x86_64-pc-linux-gnu --host=x86_64-unknown-linux-gnu \
        --prefix=/usr --with-build-python="$BUILD_PY" --disable-ipv6 \
        --without-ensurepip --disable-test-modules --with-readline=readline \
        >"$PYDIR/configure.log" 2>&1) || { tail -20 "$PYDIR/configure.log"; exit 1; }
fi

say "building Python"
# I_PUSH: os.openpty()'s STREAMS fallback names it; the ioctls just fail harmlessly.
MAKEARGS=(EXTRA_CFLAGS=-DI_PUSH=0x5302)
make -C "$PYDIR/build" -j"$JOBS" "${MAKEARGS[@]}" >"$PYDIR/make.log" 2>&1 \
    || { grep -a -E "(error|fatal error):" "$PYDIR/make.log" | head -20; exit 1; }

say "installing and pruning"
rm -rf "$PYDIR/stage" "$PYDIR/rootfs"
make -C "$PYDIR/build" install DESTDIR="$PYDIR/stage" "${MAKEARGS[@]}" >"$PYDIR/install.log" 2>&1 \
    || { tail -20 "$PYDIR/install.log"; exit 1; }
mkdir -p "$PYDIR/rootfs"
cp -a "$PYDIR/stage/usr" "$PYDIR/rootfs/usr"
P="$PYDIR/rootfs/usr"
rm -rf "$P/include" "$P/share" "$P/lib/pkgconfig" "$P/lib/libpython$MM.a" "$P/bin/idle3"* "$P/bin/"*-config
(cd "$P/lib/python$MM" && rm -rf config-"$MM"* idlelib turtledemo tkinter ensurepip test)
find "$P/lib" -type d \( -name test -o -name tests \) -prune -exec rm -rf {} +
find "$P/lib" -name __pycache__ -prune -exec rm -rf {} +
strip --strip-unneeded "$P/bin/python$MM" "$P/lib/python$MM"/lib-dynload/*.so
# Bytecode, so the first import on the target does not have to compile everything.
"$BUILD_PY" -m compileall -q -j0 "$P/lib/python$MM" >/dev/null 2>&1 || true
ln -sf "python$MM" "$P/bin/python3"
ln -sf "python$MM" "$P/bin/python"
say "done: $(du -sh "$P" | cut -f1) in $P"
