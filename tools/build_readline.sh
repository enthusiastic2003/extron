#!/bin/bash
set -euo pipefail

SYSROOT="$(pwd)/usr/mlibc-sysroot"
export CC=/home/sirjanh/extron-toolkit/toolchain/bin/aarch64-extron-gcc

export CFLAGS="--sysroot=$SYSROOT -O2 -I$(pwd)/third_party/ncurses-6.4/include -I$(pwd)/third_party/ncurses-6.4/include/ncurses -fPIE"
export LDFLAGS="--sysroot=$SYSROOT -L$SYSROOT/lib -L$(pwd)/third_party/ncurses-6.4/lib"

echo "=== Building readline ==="
cd third_party/readline-8.2
make clean > /dev/null || true
./configure --host=aarch64-linux-gnu \
  --with-curses \
  --disable-static \
  --enable-shared \
  CC="$CC" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" > /dev/null
make -j8 > /dev/null

echo "=== Copying readline into SYSROOT and RootFS ==="
# Readline creates shlib/libreadline.so.8.2 and libhistory.so.8.2
cp shlib/libreadline.so.8.2 "$SYSROOT/lib/"
cp shlib/libhistory.so.8.2 "$SYSROOT/lib/"

cd "$SYSROOT/lib"
ln -sf libreadline.so.8.2 libreadline.so.8
ln -sf libreadline.so.8 libreadline.so
ln -sf libhistory.so.8.2 libhistory.so.8
ln -sf libhistory.so.8 libhistory.so
cd - > /dev/null

# Also copy headers so bash can find them
mkdir -p "$SYSROOT/usr/include/readline"
cp *.h "$SYSROOT/usr/include/readline/"

cd ../..
echo "Done! Readline is built and staged."
