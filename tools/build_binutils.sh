#!/usr/bin/env bash
set -euo pipefail

VERSION=2.42
TARGET=aarch64-extron
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
TOOLKIT_DIR=${EXTRON_TOOLKIT:-"$REPO_DIR/../extron-toolkit"}
ARCHIVE=${BINUTILS_ARCHIVE:-"$TOOLKIT_DIR/src/binutils-$VERSION.tar.xz"}
SOURCE_DIR=${BINUTILS_SOURCE_DIR:-"$REPO_DIR/third_party/binutils-$VERSION"}
BUILD_DIR=${BINUTILS_BUILD_DIR:-"$REPO_DIR/build/binutils-$VERSION-native"}
STAGE_DIR=${BINUTILS_STAGE_DIR:-"$REPO_DIR/build/binutils-stage"}
SYSROOT=${MLIBC_SYSROOT:-"$REPO_DIR/usr/mlibc-sysroot"}
CROSS_PREFIX=${CROSS_PREFIX:-"$TOOLKIT_DIR/toolchain/bin/$TARGET-"}
PATCH_FILE="$REPO_DIR/patches/binutils/0001-aarch64-extron-target.patch"
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}

fail() {
    echo "build_binutils: $*" >&2
    exit 1
}

for tool in gcc g++ ar ranlib strip readelf; do
    [ -x "${CROSS_PREFIX}${tool}" ] || fail "missing ${CROSS_PREFIX}${tool}"
done
[ -f "$SYSROOT/lib/libc.so" ] || fail "missing dynamic mlibc at $SYSROOT/lib/libc.so"
[ -f "$PATCH_FILE" ] || fail "missing $PATCH_FILE"

if [ ! -d "$SOURCE_DIR" ]; then
    [ -f "$ARCHIVE" ] || fail "missing source archive $ARCHIVE (set BINUTILS_ARCHIVE to override)"
    mkdir -p "$SOURCE_DIR"
    echo "Extracting Binutils $VERSION..."
    tar -xJf "$ARCHIVE" --strip-components=1 -C "$SOURCE_DIR"
fi

[ -x "$SOURCE_DIR/configure" ] || fail "$SOURCE_DIR is not a Binutils $VERSION source tree"

if patch --dry-run -s -d "$SOURCE_DIR" -p1 < "$PATCH_FILE"; then
    echo "Applying Extron target patch..."
    patch -s -d "$SOURCE_DIR" -p1 < "$PATCH_FILE"
elif patch --dry-run -R -s -d "$SOURCE_DIR" -p1 < "$PATCH_FILE"; then
    echo "Extron target patch is already applied."
else
    fail "source tree does not match pristine or patched Binutils $VERSION"
fi

mkdir -p "$BUILD_DIR"
export PATH="$(dirname -- "$CROSS_PREFIX"):$PATH"
export CC="${CROSS_PREFIX}gcc"
export CXX="${CROSS_PREFIX}g++"
export AR="${CROSS_PREFIX}ar"
export RANLIB="${CROSS_PREFIX}ranlib"
export STRIP="${CROSS_PREFIX}strip"
export CFLAGS="--sysroot=$SYSROOT -O2"
export CXXFLAGS="--sysroot=$SYSROOT -O2"
# The explicit -L must precede GCC's configured static-only toolkit sysroot.
# Otherwise libc.a is selected and its coarse object grouping collides with
# libiberty's bundled getopt. Using the repository's libc.so is also the ABI
# we actually deploy and avoids any Binutils or mlibc source workaround.
export LDFLAGS="--sysroot=$SYSROOT -L$SYSROOT/lib"

echo "Configuring native Binutils $VERSION for $TARGET..."
(
    cd "$BUILD_DIR"
    "$SOURCE_DIR/configure" \
        --build="$("$SOURCE_DIR/config.guess")" \
        --host="$TARGET" \
        --target="$TARGET" \
        --prefix=/usr \
        --disable-nls \
        --disable-werror \
        --disable-gdb \
        --disable-gdbserver \
        --disable-gprofng \
        --disable-gold \
        --disable-sim \
        --disable-libdecnumber \
        --disable-readline \
        --disable-shared \
        --enable-static \
        --without-debuginfod \
        --without-zstd \
        --without-system-zlib
)

echo "Building native Binutils with $JOBS jobs..."
make -C "$BUILD_DIR" -j"$JOBS" all-binutils all-gas all-ld

echo "Staging native Binutils in $STAGE_DIR/usr/bin..."
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/usr/bin"

install_tool() {
    local source=$1
    local name=$2
    [ -x "$source" ] || fail "expected build output is missing: $source"
    install -m 0755 "$source" "$STAGE_DIR/usr/bin/$name"
    "${CROSS_PREFIX}strip" "$STAGE_DIR/usr/bin/$name"
}

install_tool "$BUILD_DIR/gas/as-new" as
install_tool "$BUILD_DIR/ld/ld-new" ld
install_tool "$BUILD_DIR/binutils/ar" ar
install_tool "$BUILD_DIR/binutils/ranlib" ranlib
install_tool "$BUILD_DIR/binutils/nm-new" nm
install_tool "$BUILD_DIR/binutils/objcopy" objcopy
install_tool "$BUILD_DIR/binutils/objdump" objdump
install_tool "$BUILD_DIR/binutils/readelf" readelf
install_tool "$BUILD_DIR/binutils/strip-new" strip
install_tool "$BUILD_DIR/binutils/strings" strings
install_tool "$BUILD_DIR/binutils/size" size
install_tool "$BUILD_DIR/binutils/addr2line" addr2line
install_tool "$BUILD_DIR/binutils/cxxfilt" 'c++filt'

for binary in "$STAGE_DIR"/usr/bin/*; do
    "${CROSS_PREFIX}readelf" -l "$binary" | grep -q '/lib/ld.so' \
        || fail "$binary does not use Extron's /lib/ld.so"
done

echo "Native Binutils is built and staged."
echo "Run 'make package-binutils' in $REPO_DIR to package it into initrd.ext2."
