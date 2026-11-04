#!/bin/bash
# Build the x86-64 initramfs (an ext2 image the loader hands to the kernel).
#
#   normal:  a tiny initramfs. /init (usr/x86_64/initramfs_init.c) mounts the real
#            root filesystem named by root= on the kernel command line, switches
#            to it and runs its init. It holds nothing else the system needs: a
#            BusyBox with libc and the dynamic linker (for the emergency shell if
#            the root cannot be mounted) and empty mount points. No libraries,
#            tools or configuration: those live on the root disk
#            (tools/build_x86_rootfs.sh).
#   AUTOEXIT=1 (`make check`) or no userland built: the freestanding smoke-test
#            payload from usr/x86_64/init.c as /bin/sh, no disk involved.
#
# Output: initrd.ext2 (or $1).
set -euo pipefail
OUT="${1:-initrd.ext2}"
ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT
UL="${EXTRON_USERLAND:-$(pwd)/build-x86_64/userland}"
mkdir -p "$ROOT"/{bin,dev,proc,sys,tmp,etc,root,lib,newroot}

if [ "${AUTOEXIT:-0}" != 1 ] && [ "${SMOKE_SHELL:-0}" != 1 ] \
        && [ -f "$UL/sysroot/usr/lib/ld.so" ] && [ -x "$UL/out/initramfs-init" ] \
        && [ -x "$UL/out/busybox" ]; then
    # ---- initramfs --------------------------------------------------------------
    install -m 755 "$UL/out/initramfs-init" "$ROOT/init"
    install -m 755 "$UL/out/busybox" "$ROOT/bin/busybox"
    ln -s busybox "$ROOT/bin/sh"
    cp "$UL/sysroot/usr/lib/ld.so" "$UL/sysroot/usr/lib/libc.so" "$ROOT/lib/"
    printf 'root:x:0:0:root:/:/bin/sh\n' > "$ROOT/etc/passwd"
    IMG_MB_DEFAULT=8
else
    # ---- bring-up / smoke-test image ----------------------------------------------
    gcc -static -nostdlib -ffreestanding -fno-pic -no-pie -fno-stack-protector \
        -fno-asynchronous-unwind-tables -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 \
        -O1 -Wall -o "$ROOT/bin/smoke" usr/x86_64/init.c
    cp "$ROOT/bin/smoke" "$ROOT/bin/sh"
    echo "extron x86-64" > "$ROOT/etc/hostname"
    printf 'root:x:0:0:root:/root:/bin/sh\n' > "$ROOT/etc/passwd"
    printf 'root:x:0:\n' > "$ROOT/etc/group"
    # AUTOEXIT=1 (used by `make check`): the smoke test resets the VM when done
    # instead of dropping into its interactive mini shell.
    [ "${AUTOEXIT:-0}" = 1 ] && touch "$ROOT/etc/autoexit"
    IMG_MB_DEFAULT=4
fi

rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1M count=${IMG_MB:-$IMG_MB_DEFAULT} status=none
mkfs.ext2 -q -E revision=0 -b 1024 -d "$ROOT" "$OUT"
echo "built $OUT"
