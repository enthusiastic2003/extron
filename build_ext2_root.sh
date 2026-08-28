#!/bin/bash
set -euo pipefail

IMG="initrd.ext2"
INITRD_DIR="build/initrd"
ROOTFS_DIR="build/rootfs"
TMP_IMG=""
CMD_FILE=""

cleanup() {
    [ -z "$TMP_IMG" ] || rm -f "$TMP_IMG"
    [ -z "$CMD_FILE" ] || rm -f "$CMD_FILE"
}
trap cleanup EXIT

echo "=== Building Ext2 Root Filesystem ==="

# Clean and create rootfs layout
rm -rf "$ROOTFS_DIR"
mkdir -p "$ROOTFS_DIR/bin"
mkdir -p "$ROOTFS_DIR/lib"
mkdir -p "$ROOTFS_DIR/dev"
mkdir -p "$ROOTFS_DIR/tmp"
mkdir -p "$ROOTFS_DIR/etc"
mkdir -p "$ROOTFS_DIR/mnt/sd"
mkdir -p "$ROOTFS_DIR/opt/doom"
mkdir -p "$ROOTFS_DIR/opt/tests"
mkdir -p "$ROOTFS_DIR/root"
mkdir -p "$ROOTFS_DIR/usr/local"
mkdir -p "$ROOTFS_DIR/usr/bin"
mkdir -p "$ROOTFS_DIR/usr/include"

# The real mlibc runtime linker and its first shared dependency. Keep these
# sourced from the checked sysroot so the compiler and root filesystem always
# use the exact same ABI build.
cp "usr/mlibc-sysroot/lib/ld.so" "$ROOTFS_DIR/lib/ld.so"
cp "usr/mlibc-sysroot/lib/libc.so" "$ROOTFS_DIR/lib/libc.so"
cp -P usr/mlibc-sysroot/lib/libreadline.so* "$ROOTFS_DIR/lib/" || true
cp -P usr/mlibc-sysroot/lib/libhistory.so* "$ROOTFS_DIR/lib/" || true
if compgen -G "$INITRD_DIR/lib/*.so" >/dev/null; then
    cp "$INITRD_DIR"/lib/*.so "$ROOTFS_DIR/opt/tests/"
fi

# Distribute system binaries to /bin
if [ -f "$INITRD_DIR/sh" ]; then
    cp "$INITRD_DIR/sh" "$ROOTFS_DIR/bin/busybox"
    # Create symlinks that debugfs will process
    # debugfs symlink syntax requires us to create them explicitly in the debugfs script later,
    # but for local structure we can just make standard symlinks or handle them in the script.
fi

if [ -f "$INITRD_DIR/nano" ]; then
    cp "$INITRD_DIR/nano" "$ROOTFS_DIR/bin/nano"
fi

# Distribute Doom
if [ -f "$INITRD_DIR/doom.elf" ]; then
    cp "$INITRD_DIR/doom.elf" "$ROOTFS_DIR/opt/doom/doom"
fi
if [ -f "$INITRD_DIR/doom1.wad" ]; then
    cp "$INITRD_DIR/doom1.wad" "$ROOTFS_DIR/opt/doom/doom1.wad"
fi

# Distribute tests
if [ -d "$INITRD_DIR/tests" ]; then
    cp -r "$INITRD_DIR/tests"/* "$ROOTFS_DIR/opt/tests/" 2>/dev/null || true
fi
# hello.txt is used as a fixture by several regression tests via relative path.
# Place it in /opt/tests/ so tests running from that directory find it.
if [ -f "$INITRD_DIR/hello.txt" ]; then
    cp "$INITRD_DIR/hello.txt" "$ROOTFS_DIR/opt/tests/hello.txt"
fi

# Distribute home/root binaries and files
for f in pty.elf reboot.elf getenv_test.elf hello.txt; do
    if [ -f "$INITRD_DIR/$f" ]; then
        cp "$INITRD_DIR/$f" "$ROOTFS_DIR/root/$f"
    fi
done

# Distribute standalone utilities to /bin
if [ -f "$INITRD_DIR/resize.elf" ]; then
    cp "$INITRD_DIR/resize.elf" "$ROOTFS_DIR/bin/resize"
fi
if [ -f "$INITRD_DIR/ldd.elf" ]; then
    cp "$INITRD_DIR/ldd.elf" "$ROOTFS_DIR/bin/ldd"
fi

# Distribute terminfo and other usr/local files
if [ -d "$INITRD_DIR/usr" ]; then
    cp -r "$INITRD_DIR/usr"/* "$ROOTFS_DIR/usr/" 2>/dev/null || true
fi
mkdir -p "$ROOTFS_DIR/usr/lib"
if compgen -G "third_party/ncurses-6.4/lib/*.so*" >/dev/null; then
    cp -a third_party/ncurses-6.4/lib/*.so* "$ROOTFS_DIR/usr/lib/"
fi
if [ -d "$INITRD_DIR/local" ]; then
    cp -r "$INITRD_DIR/local"/* "$ROOTFS_DIR/usr/local/" 2>/dev/null || true
fi

# Native Binutils is intentionally built separately: its upstream tree is
# large and a normal kernel rebuild must not silently rebuild a toolchain.
# tools/build_binutils.sh populates this staging directory when requested.
if [ -d "build/binutils-stage/usr/bin" ]; then
    cp -a build/binutils-stage/usr/bin/. "$ROOTFS_DIR/usr/bin/"
    mkdir -p "$ROOTFS_DIR/opt/tests"
    cp usr/binutils_tests/native_binutils_smoke.sh \
        "$ROOTFS_DIR/opt/tests/native_binutils_smoke.sh"
    chmod 0755 "$ROOTFS_DIR/opt/tests/native_binutils_smoke.sh"
fi

# Native GCC is also an explicit, separately built payload.  The compact
# staging tree contains the AArch64 driver/front ends, libgcc, libstdc++, and
# C++ headers.  A compiler additionally needs the libc development surface;
# keep those headers and static/startup objects out of ordinary runtime images
# unless GCC has actually been staged.
if [ -d "build/gcc-native-root/usr/bin" ]; then
    cp -a build/gcc-native-root/usr/. "$ROOTFS_DIR/usr/"
    cp -a usr/mlibc-sysroot/usr/include/. "$ROOTFS_DIR/usr/include/"
    cp -a usr/mlibc-sysroot/lib/*.a "$ROOTFS_DIR/usr/lib/"
    cp -a usr/mlibc-sysroot/lib/crt0.o usr/mlibc-sysroot/lib/crt1.o \
        "$ROOTFS_DIR/usr/lib/"
    cp -P usr/mlibc-sysroot/lib/libm.so "$ROOTFS_DIR/lib/libm.so"
    ln -sf /lib/libc.so "$ROOTFS_DIR/usr/lib/libc.so"
    ln -sf /lib/libc.so "$ROOTFS_DIR/usr/lib/libm.so"
    cp usr/gcc_tests/native_gcc_smoke.sh \
        "$ROOTFS_DIR/opt/tests/native_gcc_smoke.sh"
    chmod 0755 "$ROOTFS_DIR/opt/tests/native_gcc_smoke.sh"
fi

# The new dynamic ncurses expects terminfo at /usr/share/terminfo
# but the initrd data has it in /usr/local/share/terminfo
mkdir -p "$ROOTFS_DIR/usr/share"
if [ -d "$ROOTFS_DIR/usr/local/share/terminfo" ]; then
    cp -r "$ROOTFS_DIR/usr/local/share/terminfo" "$ROOTFS_DIR/usr/share/terminfo"
fi

# Calculate required size in MB (with some buffer)
DIR_SIZE_KB=$(du -sk "$ROOTFS_DIR" | cut -f1)
# Large developer payloads need more than nominal file-data slack: ext2's
# inode tables and block metadata also consume image space.  Leave enough room
# for native compiler temporaries and linked test executables.
EXTRA_MB=16
if [ -d "build/gcc-native-root/usr/bin" ]; then EXTRA_MB=96; fi
IMG_SIZE_MB=$(( (DIR_SIZE_KB / 1024) + EXTRA_MB ))
if [ $IMG_SIZE_MB -lt 32 ]; then IMG_SIZE_MB=32; fi

# Build on a new inode, then publish atomically. This also makes rebuilding
# safe while a QEMU validation guest still has the previous image open: that
# guest retains the old inode and cannot write stale metadata into the new FS.
TMP_IMG=$(mktemp "./${IMG}.tmp.XXXXXX")
dd if=/dev/zero of="$TMP_IMG" bs=1M count=$IMG_SIZE_MB status=none
mkfs.ext2 -q -E revision=0 -b 1024 "$TMP_IMG"

# Generate debugfs commands
CMD_FILE=$(mktemp)

# Recursively write all files and directories
cd "$ROOTFS_DIR"
find . -mindepth 1 | while read -r filepath; do
    # Remove leading ./
    relpath="${filepath#./}"
    if [ -d "$relpath" ]; then
        echo "mkdir /$relpath" >> $CMD_FILE
    else
        # Follow symlinks or copy file
        if [ -L "$relpath" ]; then
            TARGET=$(readlink "$relpath")
            echo "symlink /$relpath $TARGET" >> $CMD_FILE
        else
            echo "write $ROOTFS_DIR/$relpath /$relpath" >> $CMD_FILE
        fi
    fi
done

# Add BusyBox symlinks manually
for applet in sh ls cat rm mkdir cp mv echo grep mount umount ps kill clear; do
    echo "symlink /bin/$applet busybox" >> $CMD_FILE
done

cd ../..

debugfs -w "$TMP_IMG" -f "$CMD_FILE" > /dev/null 2>&1
rm -f "$CMD_FILE"
CMD_FILE=""
mv -f "$TMP_IMG" "$IMG"
TMP_IMG=""

echo "=== $IMG created successfully ($IMG_SIZE_MB MB) ==="
