#!/bin/bash
# Build the ext2 data disk (mounted at /mnt by the kernel). If a built mlibc
# userland exists, the regression programs live here: /opt/tests (the initrd's
# /opt is a symlink to /mnt/opt) plus hello.txt. Otherwise the disk is blank.
#   build_x86_disk.sh OUT [SIZE_MB]
set -euo pipefail
OUT="$1"; MB="${2:-64}"
UL="${EXTRON_USERLAND:-$(pwd)/build-x86_64/userland}"
ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT
if [ -d "$UL/out/tests" ]; then
    mkdir -p "$ROOT/opt/tests"
    cp "$UL"/out/tests/* "$ROOT/opt/tests/"
    cp usr/hello.txt "$ROOT/hello.txt"; cp usr/hello.txt "$ROOT/opt/tests/hello.txt"
    for s in usr/x86_64/tests/*.sh usr/x86_64/tests/*.py; do [ -f "$s" ] && install -m 755 "$s" "$ROOT/opt/tests/"; done
fi
# CPython (tools/build_x86_python.sh), when built, lives at /python on the disk
# (mounted at /mnt, so /mnt/python/bin/python3). It needs a bigger image.
PYROOT="$(pwd)/build-x86_64/python/rootfs/python"
if [ -d "$PYROOT" ]; then
    cp -a "$PYROOT" "$ROOT/python"
    [ "$MB" -ge 128 ] || MB=128
fi
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
truncate -s "${MB}M" "$OUT"
mke2fs -q -t ext2 -E revision=0 -b 1024 -d "$ROOT" "$OUT"
