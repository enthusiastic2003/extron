#!/bin/bash
# Bring the regression programs on an EXISTING root disk up to date without
# touching anything else on it: rewrites /opt/tests in place (and seeds /hello.txt if missing).
# (build_x86_rootfs.sh makes a fresh image, or with KEEP=1 rebuilds the system part
# and keeps /root, /home and /mnt; this is the fast path for just the tests.)
#   refresh_disk_tests.sh DISK
set -euo pipefail
DISK="$1"
UL="${EXTRON_USERLAND:-$(pwd)/build-x86_64/userland}"
[ -d "$UL/out/tests" ] || exit 0          # no userland built: nothing to put there
CMDS="$(mktemp)"
trap 'rm -f "$CMDS"' EXIT
# debugfs's mkdir on an existing name allocates an inode and then fails to link
# it (an orphan e2fsck complains about), so only create what is missing.
isdir() { debugfs -R "stat $1" "$DISK" 2>/dev/null | grep -q "Type: directory"; }
{
    isdir opt || echo "mkdir opt"
    isdir opt/tests || echo "mkdir opt/tests"
    put() {   # put SRC DEST MODE
        echo "rm $2"
        echo "write $1 $2"
        echo "sif $2 mode 0100$3"
    }
    for f in "$UL"/out/tests/*; do [ -f "$f" ] && put "$f" "opt/tests/$(basename "$f")" 755; done
    for s in usr/x86_64/tests/*.sh usr/x86_64/tests/*.bash usr/x86_64/tests/*.py; do [ -f "$s" ] && put "$s" "opt/tests/$(basename "$s")" 755; done
    put usr/hello.txt opt/tests/hello.txt 644
    # /hello.txt is the user's sample file: seed it once, never overwrite edits.
    debugfs -R "stat hello.txt" "$DISK" 2>/dev/null | grep -q "Type: regular" \
        || put usr/hello.txt hello.txt 644
} > "$CMDS"
debugfs -w -f "$CMDS" "$DISK" >/dev/null 2>&1 || true
