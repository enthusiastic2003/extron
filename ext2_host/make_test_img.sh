#!/bin/bash
# Creates a 16MB ext2 image with known test content.
# Requires: mke2fs, debugfs (from e2fsprogs).
# No root/sudo needed — uses debugfs instead of loop-mounting.
set -euo pipefail

IMG="ext2.img"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "=== Creating $IMG ==="
dd if=/dev/zero of="$IMG" bs=1M count=16 status=none
mkfs.ext2 -q -b 1024 "$IMG"

# --- Populate test files via debugfs ---

# Small known file
echo -n "hello from ext2" > "$TMP/hello.txt"

# Nested file
echo -n "nested content" > "$TMP/nested.txt"

# Large file — 52KB, forces single-indirect blocks (> 12 * 1024 bytes)
dd if=/dev/urandom of="$TMP/big.bin" bs=1024 count=52 status=none

# Compute md5 of big.bin for the test to check
md5sum "$TMP/big.bin" | cut -d' ' -f1 > "$TMP/big.bin.md5"

# File with known permissions (will be chmod'd)
echo -n "permtest" > "$TMP/permfile.txt"

debugfs -w "$IMG" <<EOF 2>/dev/null
mkdir subdir
write $TMP/hello.txt hello.txt
write $TMP/nested.txt subdir/nested.txt
write $TMP/big.bin big.bin
write $TMP/big.bin.md5 big.bin.md5
write $TMP/permfile.txt permfile.txt
symlink link_short hello.txt
EOF

# Long symlink target — 61 chars, exceeds the 60-byte fast-path threshold
LONG_TARGET=$(python3 -c "print('a' * 61, end='')")
debugfs -w "$IMG" -R "symlink link_long $LONG_TARGET" 2>/dev/null

echo "=== $IMG created ==="
echo "Contents:"
debugfs -R 'ls -l /' "$IMG" 2>/dev/null
