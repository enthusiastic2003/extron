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
    -O1 -Wall -o "$ROOT/bin/smoke" usr/x86_64/init.c
# /bin/sh is what the kernel launches. Default: the libc-free smoke test (which
# is also a tiny shell). With AUTOEXIT unset and BusyBox built, /bin/sh becomes
# BusyBox ash instead; the smoke test stays available as /bin/smoke.
cp "$ROOT/bin/smoke" "$ROOT/bin/sh"
echo "extron x86-64" > "$ROOT/etc/hostname"
printf 'root:x:0:0:root:/root:/bin/sh\n' > "$ROOT/etc/passwd"
printf 'root:x:0:\n' > "$ROOT/etc/group"
# Optional mlibc userland (see docs/X86_64_UEFI_PORT.md): if a built sysroot and
# test programs exist, ship the runtime linker, libc and the programs.
UL="${EXTRON_USERLAND:-$(pwd)/build-x86_64/userland}"
if [ -f "$UL/sysroot/usr/lib/ld.so" ]; then
    cp "$UL/sysroot/usr/lib/ld.so" "$UL/sysroot/usr/lib/libc.so" "$ROOT/lib/"
    for f in "$UL"/out/*; do [ -f "$f" ] && install -m 755 "$f" "$ROOT/bin/$(basename "$f")"; done
    # mlibc regression programs (usr/mlibc_tests) live on the data disk
    # (tools/build_x86_disk.sh), mounted at /mnt; /opt/tests resolves there.
    if [ -d "$UL/out/tests" ]; then
        ln -s /mnt/opt "$ROOT/opt"
        cp usr/hello.txt "$ROOT/hello.txt"
        cat > "$ROOT/bin/runtests" <<'RT'
#!/bin/sh
# Run every program in /opt/tests, report per-test status.
for t in /opt/tests/*.elf; do
    case "$t" in */mlibc_syscall_test.elf) continue;; esac   # interactive (reads the keyboard)
    echo "=== $t"
    "$t" || echo "[exit status $?]"
done
echo "RUNTESTS DONE"
RT
        chmod 755 "$ROOT/bin/runtests"
    fi
    if [ -x "$ROOT/bin/busybox" ] && [ "${AUTOEXIT:-0}" != 1 ] && [ "${SMOKE_SHELL:-0}" != 1 ]; then
        rm -f "$ROOT/bin/sh"; ln -s busybox "$ROOT/bin/sh"
        mkdir -p "$ROOT/root"
    fi
fi
# AUTOEXIT=1 (used by `make check`): smoke test resets the VM when done instead of
# dropping into the interactive mini shell.
[ "${AUTOEXIT:-0}" = 1 ] && touch "$ROOT/etc/autoexit"
rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1M count=${IMG_MB:-32} status=none
mkfs.ext2 -q -E revision=0 -b 1024 -d "$ROOT" "$OUT"
echo "built $OUT"
