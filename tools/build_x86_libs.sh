#!/bin/bash
# Cross-build the C libraries CPython's optional modules want (zlib, bzip2, xz,
# zstd, libffi, ncurses, readline, sqlite) as static PIC archives into
#     build-x86_64/extlibs/prefix/{include,lib}
# with the mlibc sysroot compiler wrapper. They are linked into Python's extension
# modules, so nothing extra is needed at run time except ncurses' compiled-in
# terminal descriptions. Kept outside the sysroot because the userland build
# recreates that directory.
#
#   tools/build_x86_libs.sh              # all libraries
#   tools/build_x86_libs.sh zlib xz      # just these
# Overrides: JOBS REBUILD=1
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
UL="${EXTRON_USERLAND:-$REPO/build-x86_64/userland}"
EXT="$REPO/build-x86_64/extlibs"
DL="$EXT/dl"; WORK="$EXT/work"; PREFIX="$EXT/prefix"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
export EXTRON_SYSROOT="$UL/sysroot"
export CC="$REPO/tools/x86_64-extron-gcc"
export AR=ar RANLIB=ranlib
export CFLAGS="-O2 -fPIC"
export CPPFLAGS="-I$PREFIX/include"
export LDFLAGS="-L$PREFIX/lib"
HOST=(--build=x86_64-pc-linux-gnu --host=x86_64-unknown-linux-gnu)

[ -f "$EXTRON_SYSROOT/usr/lib/libc.so" ] || { echo "build the userland first" >&2; exit 1; }
mkdir -p "$DL" "$WORK" "$PREFIX/include" "$PREFIX/lib"

say() { printf '\n== %s\n' "$*"; }

# fetch NAME-VERSION.tar.gz URL SHA256 -> unpacked fresh in $WORK/<dir>
fetch() {
    local file="$1" url="$2" sha="$3" dir="$4"
    [ -f "$DL/$file" ] || curl -fsSL "$url" -o "$DL/$file"
    echo "$sha  $DL/$file" | sha256sum -c - >/dev/null
    rm -rf "$WORK/$dir"; mkdir -p "$WORK/$dir"
    tar xzf "$DL/$file" --strip-components=1 -C "$WORK/$dir"
}

done_marker() { echo "$EXT/.built-$1"; }
want() {   # want NAME: requested and (not built yet or REBUILD)
    local n="$1"
    if [ "$#" -gt 0 ] && [ -n "${SELECT:-}" ] && [[ " $SELECT " != *" $n "* ]]; then return 1; fi
    [ "${REBUILD:-0}" = 1 ] || [ ! -f "$(done_marker "$n")" ]
}
finish() { touch "$(done_marker "$1")"; }
SELECT="${*:-}"

build_zlib() {
    say "zlib"
    fetch zlib-1.3.1.tar.gz https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz \
        9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23 zlib
    (cd "$WORK/zlib" && ./configure --prefix="$PREFIX" --static >/dev/null && make -j"$JOBS" libz.a >/dev/null \
        && make install >/dev/null)
}

build_bzip2() {
    say "bzip2"
    fetch bzip2-1.0.8.tar.gz https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz \
        ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269 bzip2
    (cd "$WORK/bzip2" && make -j"$JOBS" libbz2.a CC="$CC" AR=ar RANLIB=ranlib \
        CFLAGS="-O2 -fPIC -D_FILE_OFFSET_BITS=64" >/dev/null \
        && install -m 644 bzlib.h "$PREFIX/include/" && install -m 644 libbz2.a "$PREFIX/lib/")
}

build_xz() {
    say "xz (liblzma)"
    fetch xz-5.8.1.tar.gz https://github.com/tukaani-project/xz/releases/download/v5.8.1/xz-5.8.1.tar.gz \
        507825b599356c10dca1cd720c9d0d0c9d5400b9de300af00e4d1ea150795543 xz
    (cd "$WORK/xz" && ./configure "${HOST[@]}" --prefix="$PREFIX" --disable-shared --enable-static \
        --disable-xz --disable-xzdec --disable-lzmadec --disable-lzmainfo --disable-lzma-links \
        --disable-scripts --disable-doc --disable-nls --disable-threads --disable-assembler \
        >"$WORK/xz.configure.log" 2>&1 && make -j"$JOBS" >"$WORK/xz.make.log" 2>&1 \
        && make install >/dev/null 2>&1)
}

build_zstd() {
    say "zstd"
    fetch zstd-1.5.7.tar.gz https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz \
        eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3 zstd
    (cd "$WORK/zstd/lib" && make -j"$JOBS" libzstd.a CC="$CC" AR=ar RANLIB=ranlib \
        CFLAGS="-O2 -fPIC" ZSTD_LEGACY_SUPPORT=0 HAVE_PTHREAD=0 HAVE_ZLIB=0 HAVE_LZMA=0 HAVE_LZ4=0 \
        >"$WORK/zstd.make.log" 2>&1 \
        && install -m 644 libzstd.a "$PREFIX/lib/" \
        && install -m 644 zstd.h zdict.h zstd_errors.h "$PREFIX/include/")
}

build_libffi() {
    say "libffi"
    fetch libffi-3.4.6.tar.gz https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz \
        b0dea9df23c863a7a50e825440f3ebffabd65df1497108e5d437747843895a4e libffi
    # libffi picks its closure-memory strategy under #ifdef __linux__, which the
    # compiler wrapper undefines: ask for an executable anonymous mapping explicitly,
    # or closures would live in non-executable malloc memory.
    (cd "$WORK/libffi" && CFLAGS="$CFLAGS -DFFI_MMAP_EXEC_WRIT=1" ./configure "${HOST[@]}" --prefix="$PREFIX" --disable-shared --enable-static \
        --disable-docs --disable-multi-os-directory --disable-exec-static-tramp \
        >"$WORK/libffi.configure.log" 2>&1 && make -j"$JOBS" CFLAGS="$CFLAGS -DFFI_MMAP_EXEC_WRIT=1" >"$WORK/libffi.make.log" 2>&1 \
        && make install >/dev/null 2>&1)
    # install puts headers under lib/libffi-*/include on some layouts: normalise.
    for h in "$PREFIX"/lib/libffi-*/include/*.h; do [ -f "$h" ] && cp -f "$h" "$PREFIX/include/"; done
    return 0
}

build_ncurses() {
    say "ncurses (wide)"
    fetch ncurses-6.5.tar.gz https://ftp.gnu.org/gnu/ncurses/ncurses-6.5.tar.gz \
        136d91bc269a9a5785e5f9e980bc76ab57428f604ce3e5a5a90cebc767971cc6 ncurses
    # Terminal descriptions are compiled into the library (--with-fallbacks) with the
    # host's tic/infocmp, so no terminfo database has to be installed on the target.
    (cd "$WORK/ncurses" && ./configure "${HOST[@]}" --prefix="$PREFIX" --without-shared --with-normal \
        --without-debug --without-cxx --without-cxx-binding --without-ada --without-manpages \
        --without-tests --without-progs --enable-widec --enable-overwrite --disable-stripping \
        --without-gpm --without-sysmouse --disable-database --with-fallbacks="vt100,vt102,linux,xterm,xterm-256color,screen,ansi,dumb" \
        --with-tic-path=/usr/bin/tic --with-infocmp-path=/usr/bin/infocmp --with-build-cc=gcc \
        --disable-home-terminfo --disable-db-install --with-pkg-config=no \
        >"$WORK/ncurses.configure.log" 2>&1 && make -j"$JOBS" >"$WORK/ncurses.make.log" 2>&1 \
        && make install.libs install.includes >"$WORK/ncurses.install.log" 2>&1)
}

build_readline() {
    say "readline"
    fetch readline-8.2.tar.gz https://ftp.gnu.org/gnu/readline/readline-8.2.tar.gz \
        3feb7171f16a84ee82ca18a36d7b9be109a52c04f492a053331d7d1095007c35 readline
    (cd "$WORK/readline" && bash_cv_wcwidth_broken=no bash_cv_func_sigsetjmp=present \
        bash_cv_func_strcoll_broken=no bash_cv_must_reinstall_sighandlers=no bash_cv_func_ctype_nonascii=yes \
        bash_cv_unusable_rtsigs=no bash_cv_getcwd_malloc=yes \
        LIBS="-lncursesw" ./configure "${HOST[@]}" --prefix="$PREFIX" --disable-shared --enable-static \
        --with-curses >"$WORK/readline.configure.log" 2>&1 \
        && make -j"$JOBS" static >"$WORK/readline.make.log" 2>&1 \
        && make install-static install-headers >/dev/null 2>&1) || return 1
    # readline.h lives in include/readline/: also expose libreadline for -lreadline.
    return 0
}

build_sqlite() {
    say "sqlite"
    fetch sqlite-autoconf-3500400.tar.gz https://www.sqlite.org/2025/sqlite-autoconf-3500400.tar.gz \
        a3db587a1b92ee5ddac2f66b3edb41b26f9c867275782d46c3a088977d6a5b18 sqlite
    (cd "$WORK/sqlite" && ./configure "${HOST[@]}" --prefix="$PREFIX" --disable-shared --enable-static \
        --disable-readline --disable-load-extension >"$WORK/sqlite.configure.log" 2>&1 \
        && make -j"$JOBS" libsqlite3.a >"$WORK/sqlite.make.log" 2>&1 \
        && install -m 644 libsqlite3.a "$PREFIX/lib/" && install -m 644 sqlite3.h sqlite3ext.h "$PREFIX/include/")
}

for lib in zlib bzip2 xz zstd libffi ncurses readline sqlite; do
    if want "$lib"; then
        "build_$lib" && finish "$lib" || { echo "FAILED: $lib (logs in $WORK)" >&2; exit 1; }
    fi
done
say "libraries in $PREFIX: $(ls "$PREFIX/lib" | grep -c '\.a$') archives"
