#!/bin/bash
set -euo pipefail

IMG="initrd.ext2"
INITRD_DIR="build/initrd"
ROOTFS_DIR="build/rootfs"

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

# The real mlibc runtime linker and its first shared dependency. Keep these
# sourced from the checked sysroot so the compiler and root filesystem always
# use the exact same ABI build.
cp "usr/mlibc-sysroot/lib/ld.so" "$ROOTFS_DIR/lib/ld.so"
cp "usr/mlibc-sysroot/lib/libc.so" "$ROOTFS_DIR/lib/libc.so"
if [ -f "$INITRD_DIR/lib/libextron_rtld_test.so" ]; then
    cp "$INITRD_DIR/lib/libextron_rtld_test.so" "$ROOTFS_DIR/lib/"
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

# Distribute terminfo and other usr/local files
if [ -d "$INITRD_DIR/usr" ]; then
    cp -r "$INITRD_DIR/usr"/* "$ROOTFS_DIR/usr/" 2>/dev/null || true
fi
if [ -d "$INITRD_DIR/local" ]; then
    cp -r "$INITRD_DIR/local"/* "$ROOTFS_DIR/usr/local/" 2>/dev/null || true
fi

# Calculate required size in MB (with some buffer)
DIR_SIZE_KB=$(du -sk "$ROOTFS_DIR" | cut -f1)
IMG_SIZE_MB=$(( (DIR_SIZE_KB / 1024) + 16 )) # 16MB extra space
if [ $IMG_SIZE_MB -lt 32 ]; then IMG_SIZE_MB=32; fi

dd if=/dev/zero of="$IMG" bs=1M count=$IMG_SIZE_MB status=none
mkfs.ext2 -q -E revision=0 -b 1024 "$IMG"

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

debugfs -w "$IMG" -f $CMD_FILE > /dev/null 2>&1
rm -f $CMD_FILE

echo "=== $IMG created successfully ($IMG_SIZE_MB MB) ==="
