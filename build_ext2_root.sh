#!/bin/bash
set -euo pipefail

IMG="initrd.ext2"
INITRD_DIR="build/initrd"

echo "=== Building Ext2 Root Filesystem ==="
# Calculate required size in MB (with some buffer)
DIR_SIZE_KB=$(du -sk "$INITRD_DIR" | cut -f1)
IMG_SIZE_MB=$(( (DIR_SIZE_KB / 1024) + 16 )) # 16MB extra space
if [ $IMG_SIZE_MB -lt 32 ]; then IMG_SIZE_MB=32; fi

dd if=/dev/zero of="$IMG" bs=1M count=$IMG_SIZE_MB status=none
mkfs.ext2 -q -E revision=0 -b 1024 "$IMG"

# Generate debugfs commands
CMD_FILE=$(mktemp)

# Create base directories
echo "mkdir /dev" >> $CMD_FILE
echo "mkdir /tmp" >> $CMD_FILE
echo "mkdir /etc" >> $CMD_FILE
echo "mkdir /mnt" >> $CMD_FILE
echo "mkdir /mnt/sd" >> $CMD_FILE

# Recursively write all files and directories
cd "$INITRD_DIR"
find . -mindepth 1 | while read -r filepath; do
    # Remove leading ./
    relpath="${filepath#./}"
    if [ -d "$relpath" ]; then
        echo "mkdir /$relpath" >> $CMD_FILE
    else
        echo "write $INITRD_DIR/$relpath /$relpath" >> $CMD_FILE
    fi
done
cd ../..

debugfs -w "$IMG" -f $CMD_FILE > /dev/null 2>&1
rm -f $CMD_FILE

echo "=== $IMG created successfully ($IMG_SIZE_MB MB) ==="
