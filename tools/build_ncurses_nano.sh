#!/bin/bash
set -euo pipefail

SYSROOT="$(pwd)/usr/mlibc-sysroot"
NCURSES_PATCH="$(pwd)/patches/ncurses/0001-config-sub-extron.patch"
export CC=/home/sirjanh/extron-toolkit/toolchain/bin/aarch64-extron-gcc
export CFLAGS="--sysroot=$SYSROOT -O2"
# No need for -nostartfiles or -dynamic-linker anymore, the toolchain specs handle it!
export LDFLAGS="--sysroot=$SYSROOT -L$SYSROOT/lib"

echo "=== Building ncurses (shared) ==="
cd third_party/ncurses-6.4
if patch --dry-run -s -p1 < "$NCURSES_PATCH"; then
  patch -s -p1 < "$NCURSES_PATCH"
elif ! patch --dry-run -R -s -p1 < "$NCURSES_PATCH"; then
  echo "ncurses source does not match the expected pristine or patched 6.4 tree" >&2
  exit 1
fi
make clean > /dev/null || true
./configure --host=aarch64-linux-gnu \
  --with-shared \
  --without-cxx --without-ada --without-tests --without-progs \
  --enable-static \
  CC="$CC" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" > /dev/null
make -j8 > /dev/null
cd ../..

echo "=== Building nano ==="
export LDFLAGS="--sysroot=$SYSROOT -L$SYSROOT/lib -L$(pwd)/third_party/ncurses-6.4/lib -pie"
export CFLAGS="--sysroot=$SYSROOT -O2 -I$(pwd)/third_party/ncurses-6.4/include -I$(pwd)/third_party/ncurses-6.4/include/ncurses -fPIE"

cd third_party/nano-7.2
make clean > /dev/null || true
./configure --host=aarch64-linux-gnu \
  --disable-utf8 --enable-tiny --disable-libmagic --disable-color \
  --disable-speller --disable-mouse \
  CC="$CC" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" > /dev/null
make -j8 > /dev/null

# Copy built nano into the OS staging area
cp src/nano ../../usr/nano
cd ../..

echo "Done!"
