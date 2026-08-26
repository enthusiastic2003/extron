#!/bin/bash
set -euo pipefail

SYSROOT="$(pwd)/usr/mlibc-sysroot"
export CC=/home/sirjanh/extron-toolkit/toolchain/bin/aarch64-extron-gcc
export CFLAGS="--sysroot=$SYSROOT -O2"
# -nostartfiles to avoid linking crt0.o into shared libraries due to toolchain spec bug
export LDFLAGS="--sysroot=$SYSROOT -L$SYSROOT/lib -Wl,--dynamic-linker=/lib/ld.so -nostartfiles"

echo "=== Building ncurses (shared) ==="
cd third_party/ncurses-6.4
make clean > /dev/null || true
./configure --host=aarch64-linux-gnu \
  --with-shared \
  --without-cxx --without-ada --without-tests --without-progs \
  --enable-static \
  CC="$CC" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" > /dev/null
make -j8 > /dev/null
cd ../..

echo "=== Building nano ==="
# nano is an executable, so we MUST link with standard start files. We remove -nostartfiles here.
export LDFLAGS="--sysroot=$SYSROOT -L$SYSROOT/lib -Wl,--dynamic-linker=/lib/ld.so -L$(pwd)/third_party/ncurses-6.4/lib -pie"
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
