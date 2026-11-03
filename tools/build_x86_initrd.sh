#!/bin/bash
# Build a minimal ext2 root for the x86-64 bring-up: /bin/sh is the freestanding
# smoke-test payload from usr/x86_64/init.c. Output: initrd.ext2 (or $1).
set -euo pipefail
OUT="${1:-initrd.ext2}"
ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT
mkdir -p "$ROOT"/{bin,dev,tmp,etc,root,lib}
gcc -static -nostdlib -ffreestanding -fno-pic -no-pie -fno-stack-protector \
    -fno-asynchronous-unwind-tables -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 \
    -O1 -Wall -o "$ROOT/bin/sh" usr/x86_64/init.c
echo "extron x86-64" > "$ROOT/etc/hostname"
# AUTOEXIT=1 (used by `make check`): smoke test resets the VM when done instead of
# dropping into the interactive mini shell.
[ "${AUTOEXIT:-0}" = 1 ] && touch "$ROOT/etc/autoexit"
rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1M count=8 status=none
mkfs.ext2 -q -E revision=0 -b 1024 -d "$ROOT" "$OUT"
echo "built $OUT"
