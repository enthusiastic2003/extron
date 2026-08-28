#!/usr/bin/env bash
set -euo pipefail

VERSION=16.2.0
TARGET=aarch64-extron
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
TOOLKIT_DIR=${EXTRON_TOOLKIT:-"$REPO_DIR/../extron-toolkit"}
ARCHIVE=${GCC_ARCHIVE:-"$TOOLKIT_DIR/src/gcc-$VERSION.tar.xz"}
PREREQ_ARCHIVE_DIR=${GCC_PREREQ_ARCHIVE_DIR:-"$TOOLKIT_DIR/src/gcc-$VERSION"}
SOURCE_DIR=${GCC_SOURCE_DIR:-"$REPO_DIR/third_party/gcc-$VERSION"}
SYSROOT=${MLIBC_SYSROOT:-"$REPO_DIR/usr/mlibc-sysroot"}
BINUTILS_PREFIX=${CROSS_PREFIX:-"$TOOLKIT_DIR/toolchain/bin/$TARGET-"}
PATCH_FILE="$REPO_DIR/patches/gcc/0001-aarch64-extron-target.patch"

CROSS_BUILD=${GCC_CROSS_BUILD_DIR:-"$REPO_DIR/build/gcc-cross-runtime"}
CROSS_STAGE=${GCC_CROSS_STAGE_DIR:-"$REPO_DIR/build/gcc-cross-stage"}
DEPS_BUILD=${GCC_DEPS_BUILD_DIR:-"$REPO_DIR/build/gcc-native-deps"}
DEPS_STAGE=${GCC_DEPS_STAGE_DIR:-"$REPO_DIR/build/gcc-native-deps-stage"}
NATIVE_BUILD=${GCC_NATIVE_BUILD_DIR:-"$REPO_DIR/build/gcc-native-host"}
NATIVE_STAGE=${GCC_NATIVE_STAGE_DIR:-"$REPO_DIR/build/gcc-native-stage"}
ROOT_STAGE=${GCC_ROOT_STAGE_DIR:-"$REPO_DIR/build/gcc-native-root"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}

fail() {
    echo "build_gcc: $*" >&2
    exit 1
}

extract_prerequisite() {
    local archive=$1
    local destination=$2
    [ -d "$destination" ] && return
    [ -f "$archive" ] || fail "missing prerequisite archive $archive"
    mkdir -p "$destination"
    case "$archive" in
        *.tar.bz2) tar -xjf "$archive" --strip-components=1 -C "$destination" ;;
        *.tar.gz)  tar -xzf "$archive" --strip-components=1 -C "$destination" ;;
        *) fail "unsupported prerequisite archive $archive" ;;
    esac
}

for tool in as ld ar ranlib nm strip readelf; do
    [ -x "${BINUTILS_PREFIX}${tool}" ] || fail "missing ${BINUTILS_PREFIX}${tool}"
done
[ -f "$ARCHIVE" ] || fail "missing GCC archive $ARCHIVE"
[ -f "$PATCH_FILE" ] || fail "missing Extron GCC patch"
[ -f "$SYSROOT/lib/ld.so" ] || fail "mlibc sysroot has not been built"

if [ ! -d "$SOURCE_DIR" ]; then
    mkdir -p "$SOURCE_DIR"
    echo "Extracting GCC $VERSION..."
    tar -xJf "$ARCHIVE" --strip-components=1 -C "$SOURCE_DIR"
fi

extract_prerequisite "$PREREQ_ARCHIVE_DIR/gmp-6.3.0.tar.bz2" "$SOURCE_DIR/gmp"
extract_prerequisite "$PREREQ_ARCHIVE_DIR/mpfr-4.2.2.tar.bz2" "$SOURCE_DIR/mpfr"
extract_prerequisite "$PREREQ_ARCHIVE_DIR/mpc-1.3.1.tar.gz" "$SOURCE_DIR/mpc"

if patch --dry-run -s -d "$SOURCE_DIR" -p1 < "$PATCH_FILE"; then
    echo "Applying Extron GCC target patch..."
    patch -s -d "$SOURCE_DIR" -p1 < "$PATCH_FILE"
elif patch --dry-run -R -s -d "$SOURCE_DIR" -p1 < "$PATCH_FILE"; then
    echo "Extron GCC target patch is already applied."
else
    fail "source tree does not match pristine or patched GCC $VERSION"
fi

export PATH="$(dirname -- "$BINUTILS_PREFIX"):$PATH"
export CCACHE_DISABLE=1
BUILD_TRIPLET=$($SOURCE_DIR/config.guess)

common_configure=(
    --target="$TARGET"
    --prefix=/usr
    --with-native-system-header-dir=/usr/include
    --enable-languages=c,c++,lto
    --enable-threads=posix
    --enable-shared
    --disable-bootstrap
    --disable-multilib
    --disable-nls
    --disable-werror
    --disable-libsanitizer
    --disable-libssp
    --disable-libquadmath
    --disable-libgomp
    --disable-libatomic
    --without-isl
    --without-zstd
)

mkdir -p "$CROSS_BUILD"
if [ ! -f "$CROSS_BUILD/Makefile" ]; then
    echo "Configuring the Extron cross compiler..."
    (cd "$CROSS_BUILD" && "$SOURCE_DIR/configure" \
        --build="$BUILD_TRIPLET" --host="$BUILD_TRIPLET" \
        --with-sysroot="$SYSROOT" "${common_configure[@]}")
fi

echo "Building cross GCC, libgcc, and libstdc++..."
make -C "$CROSS_BUILD" -j"$JOBS" \
    all-gcc all-target-libgcc all-target-libstdc++-v3
rm -rf "$CROSS_STAGE"
make -C "$CROSS_BUILD" install-gcc install-target-libgcc \
    install-target-libstdc++-v3 DESTDIR="$CROSS_STAGE"

CROSS_CC="$CROSS_STAGE/usr/bin/$TARGET-gcc -B$BINUTILS_PREFIX"
CROSS_CXX="$CROSS_STAGE/usr/bin/$TARGET-g++ -B$BINUTILS_PREFIX"
rm -rf "$DEPS_BUILD" "$DEPS_STAGE"
mkdir -p "$DEPS_BUILD" "$DEPS_STAGE"

echo "Building target GMP..."
mkdir -p "$DEPS_BUILD/gmp"
(cd "$DEPS_BUILD/gmp" && CC="$CROSS_CC" CXX="$CROSS_CXX" \
    CFLAGS='-O2 -fPIC -std=gnu17' CXXFLAGS='-O2 -fPIC' \
    "$SOURCE_DIR/gmp/configure" --build="$BUILD_TRIPLET" --host="$TARGET" \
        --prefix=/usr --disable-shared --enable-static --with-pic)
make -C "$DEPS_BUILD/gmp" -j"$JOBS"
make -C "$DEPS_BUILD/gmp" install DESTDIR="$DEPS_STAGE"

echo "Building target MPFR..."
mkdir -p "$DEPS_BUILD/mpfr"
(cd "$DEPS_BUILD/mpfr" && CC="$CROSS_CC" CFLAGS='-O2 -fPIC -std=gnu17' \
    CPPFLAGS="-I$DEPS_STAGE/usr/include" LDFLAGS="-L$DEPS_STAGE/usr/lib" \
    "$SOURCE_DIR/mpfr/configure" --build="$BUILD_TRIPLET" --host="$TARGET" \
        --prefix=/usr --disable-shared --enable-static --with-pic \
        --with-gmp="$DEPS_STAGE/usr")
make -C "$DEPS_BUILD/mpfr" -j"$JOBS"
make -C "$DEPS_BUILD/mpfr" install DESTDIR="$DEPS_STAGE"

echo "Building target MPC..."
mkdir -p "$DEPS_BUILD/mpc"
(cd "$DEPS_BUILD/mpc" && CC="$CROSS_CC" CFLAGS='-O2 -fPIC -std=gnu17' \
    CPPFLAGS="-I$DEPS_STAGE/usr/include" LDFLAGS="-L$DEPS_STAGE/usr/lib" \
    "$SOURCE_DIR/mpc/configure" --build="$BUILD_TRIPLET" --host="$TARGET" \
        --prefix=/usr --disable-shared --enable-static --with-pic \
        --with-gmp="$DEPS_STAGE/usr" --with-mpfr="$DEPS_STAGE/usr")
make -C "$DEPS_BUILD/mpc" -j"$JOBS"
make -C "$DEPS_BUILD/mpc" install DESTDIR="$DEPS_STAGE"

mkdir -p "$NATIVE_BUILD"
if [ ! -f "$NATIVE_BUILD/Makefile" ]; then
    echo "Configuring native Extron GCC (Canadian cross)..."
    (cd "$NATIVE_BUILD" && \
        CC="$CROSS_CC" CXX="$CROSS_CXX" \
        CFLAGS='-O2 -std=gnu17' CXXFLAGS='-O2' \
        CPPFLAGS="-I$DEPS_STAGE/usr/include" \
        LDFLAGS="-L$DEPS_STAGE/usr/lib" \
        AR="${BINUTILS_PREFIX}ar" AS="${BINUTILS_PREFIX}as" \
        LD="${BINUTILS_PREFIX}ld" NM="${BINUTILS_PREFIX}nm" \
        RANLIB="${BINUTILS_PREFIX}ranlib" STRIP="${BINUTILS_PREFIX}strip" \
        "$SOURCE_DIR/configure" \
            --build="$BUILD_TRIPLET" --host="$TARGET" \
            --with-sysroot=/ --with-build-sysroot="$SYSROOT" \
            --with-as=/usr/bin/as --with-ld=/usr/bin/ld \
            --with-gmp="$DEPS_STAGE/usr" --with-mpfr="$DEPS_STAGE/usr" \
            --with-mpc="$DEPS_STAGE/usr" "${common_configure[@]}")
fi

echo "Building native GCC..."
make -C "$NATIVE_BUILD" -j"$JOBS" all-gcc
rm -rf "$NATIVE_STAGE"
make -C "$NATIVE_BUILD" install-gcc DESTDIR="$NATIVE_STAGE"

echo "Assembling the deployable GCC developer payload..."
rm -rf "$ROOT_STAGE"
mkdir -p "$ROOT_STAGE/usr/bin" "$ROOT_STAGE/usr/include/c++" \
    "$ROOT_STAGE/usr/lib" "$ROOT_STAGE/usr/libexec"
cp -a "$NATIVE_STAGE/usr/bin/." "$ROOT_STAGE/usr/bin/"
cp -a "$NATIVE_STAGE/usr/libexec/." "$ROOT_STAGE/usr/libexec/"
mkdir -p "$ROOT_STAGE/usr/lib/gcc/$TARGET"
cp -a "$NATIVE_STAGE/usr/lib/gcc/$TARGET/$VERSION" \
    "$ROOT_STAGE/usr/lib/gcc/$TARGET/"
cp -a "$CROSS_STAGE/usr/lib/gcc/$TARGET/$VERSION/." \
    "$ROOT_STAGE/usr/lib/gcc/$TARGET/$VERSION/"
cp -a "$CROSS_STAGE/usr/$TARGET/include/c++/$VERSION" \
    "$ROOT_STAGE/usr/include/c++/"
cp -a "$CROSS_STAGE/usr/$TARGET/lib/." "$ROOT_STAGE/usr/lib/"

for file in "$ROOT_STAGE"/usr/bin/* \
            "$ROOT_STAGE"/usr/libexec/gcc/$TARGET/$VERSION/*; do
    if [ -f "$file" ] && "${BINUTILS_PREFIX}readelf" -h "$file" >/dev/null 2>&1; then
        "${BINUTILS_PREFIX}strip" "$file"
    fi
done

for binary in "$ROOT_STAGE/usr/bin/gcc" \
              "$ROOT_STAGE/usr/libexec/gcc/$TARGET/$VERSION/cc1" \
              "$ROOT_STAGE/usr/libexec/gcc/$TARGET/$VERSION/cc1plus"; do
    "${BINUTILS_PREFIX}readelf" -l "$binary" | grep -q '/lib/ld.so' \
        || fail "$binary does not use Extron's runtime linker"
done
[ -f "$ROOT_STAGE/usr/libexec/gcc/$TARGET/$VERSION/liblto_plugin.so" ] \
    || fail "native LTO plugin was not staged"

echo "Native GCC $VERSION is built and staged in $ROOT_STAGE."
echo "Run 'make package-gcc' in $REPO_DIR to create the developer initrd."
