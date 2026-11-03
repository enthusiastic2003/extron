#!/bin/bash
# Reproducible x86-64 userland build: mlibc (from the Extron fork) + BusyBox +
# the mlibc regression programs, using the host GCC. Everything lands in
# build-x86_64/userland/ :
#     sysroot/   mlibc headers, crt1.o, ld.so, libc.so ...
#     out/       busybox, test programs, out/tests/*.elf (+ their DSOs)
# Then tools/build_x86_initrd.sh packs it into the ext2 root.
#
# Needs: gcc/g++, python3 (+pip for meson/ninja if they are not installed),
# git, curl, patch, make, bzip2.  Network is used for the mlibc clone, the
# BusyBox tarball (sha256-pinned) and meson/ninja.
#
# Overrides: MLIBC_REPO MLIBC_REF BUSYBOX_VERSION BUSYBOX_SHA256 JOBS
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
UL="${EXTRON_USERLAND:-$REPO/build-x86_64/userland}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

MLIBC_REPO="${MLIBC_REPO:-https://github.com/enthusiastic2003/mlibc.git}"
MLIBC_REF="${MLIBC_REF:-x86}"
BUSYBOX_VERSION="${BUSYBOX_VERSION:-1.38.0}"
BUSYBOX_SHA256="${BUSYBOX_SHA256:-34f9ea6ff8636f2c9241153b9114eefa9e65674a45318ae1ef95bb5f31c53bb2}"

SRC="$UL/src"; SYSROOT="$UL/sysroot"; OUT="$UL/out"
CC="$REPO/tools/x86_64-extron-gcc"
export EXTRON_SYSROOT="$SYSROOT"
mkdir -p "$SRC" "$OUT/tests"

say() { printf '\n== %s\n' "$*"; }

# ---- build tools: meson + ninja ------------------------------------------
if ! command -v meson >/dev/null || ! command -v ninja >/dev/null; then
    say "installing meson + ninja into $UL/pytools"
    pip3 install -q --break-system-packages --target "$UL/pytools" meson ninja 2>/dev/null \
        || pip3 install -q --target "$UL/pytools" meson ninja
    export PYTHONPATH="$UL/pytools${PYTHONPATH:+:$PYTHONPATH}"
    export PATH="$UL/pytools/bin:$PATH"
fi

# ---- mlibc ------------------------------------------------------------------
say "mlibc ($MLIBC_REPO @ $MLIBC_REF)"
if [ ! -d "$SRC/mlibc/.git" ]; then
    git clone -q --branch "$MLIBC_REF" "$MLIBC_REPO" "$SRC/mlibc"
else
    git -C "$SRC/mlibc" fetch -q origin "$MLIBC_REF" && git -C "$SRC/mlibc" checkout -q FETCH_HEAD
fi
rm -rf "$UL/build-mlibc"
meson setup "$UL/build-mlibc" "$SRC/mlibc" \
    --cross-file "$REPO/third_party/mlibc/cross-x86_64-extron.ini" \
    --prefix=/usr --libdir=lib -Dbuildtype=release -Dheaders_only=false >/dev/null
ninja -C "$UL/build-mlibc" >/dev/null
rm -rf "$SYSROOT"
DESTDIR="$SYSROOT" ninja -C "$UL/build-mlibc" install >/dev/null
test -f "$SYSROOT/usr/lib/ld.so" && test -f "$SYSROOT/usr/lib/libc.so" && test -f "$SYSROOT/usr/lib/crt1.o"

# ---- BusyBox ----------------------------------------------------------------
say "BusyBox $BUSYBOX_VERSION"
TARBALL="$SRC/busybox-$BUSYBOX_VERSION.tar.bz2"
if [ ! -f "$TARBALL" ]; then
    curl -fsSL "https://busybox.net/downloads/busybox-$BUSYBOX_VERSION.tar.bz2" -o "$TARBALL"
fi
echo "$BUSYBOX_SHA256  $TARBALL" | sha256sum -c - >/dev/null
BB="$SRC/busybox"
rm -rf "$BB"; mkdir -p "$BB"
tar -xjf "$TARBALL" --strip-components=1 -C "$BB"
BUSYBOX_DIR="$BB" sh "$REPO/tools/configure_busybox.sh" >/dev/null
sed -i 's|^CONFIG_EXTRA_LDFLAGS=.*|CONFIG_EXTRA_LDFLAGS=""|' "$BB/.config"   # seed names the aarch64 sysroot
make -C "$BB" -j"$JOBS" CC="$CC" HOSTCC=gcc CCACHE_DISABLE=1 >"$UL/busybox-build.log" 2>&1 \
    || { tail -30 "$UL/busybox-build.log"; exit 1; }
install -m 755 "$BB/busybox" "$OUT/busybox"

# ---- test programs ------------------------------------------------------------
say "test programs"
for f in "$REPO"/usr/x86_64/tests/*.c; do
    n=$(basename "$f" .c)
    "$CC" -O2 "$f" -o "$OUT/$n"
done
"$CC" -O2 -static "$REPO/usr/x86_64/tests/hello.c" -o "$OUT/hello-static"

T="$REPO/usr/mlibc_tests"; D="$UL/dso"; mkdir -p "$D"
"$CC" -shared -nostdlib -Wl,-soname,libextron_rtld_dep.so,-z,relro,-z,now \
    "$T/libextron_rtld_dep.c" -o "$D/libextron_rtld_dep.so"
"$CC" -shared -nostdlib -L"$D" -Wl,-soname,libextron_rtld_test.so,-z,relro,-z,now,-rpath=/opt/tests \
    "$T/libextron_rtld_test.c" -lextron_rtld_dep -o "$D/libextron_rtld_test.so"
for f in "$T"/mlibc_*.c; do
    n=$(basename "$f" .c)
    case "$n" in
        mlibc_dynamic_test|mlibc_dlopen_test|mlibc_dlthread_test)
            "$CC" -O1 -I"$REPO/usr" "$f" -L"$D" -Wl,-rpath-link,"$D",-rpath=/opt/tests \
                -lextron_rtld_test -o "$OUT/tests/$n.elf" ;;
        mlibc_relro_test)
            "$CC" -O1 -I"$REPO/usr" "$f" -Wl,-z,relro,-z,now -o "$OUT/tests/$n.elf" ;;
        *)  "$CC" -O1 -I"$REPO/usr" "$f" -o "$OUT/tests/$n.elf" ;;
    esac
done
cp "$D"/*.so "$OUT/tests/"

say "done: $(ls "$OUT/tests"/*.elf | wc -l) test programs, busybox $(stat -c %s "$OUT/busybox") bytes"
echo "userland in $UL — now: tools/build_x86_initrd.sh && make -f Makefile.x86_64 run"
