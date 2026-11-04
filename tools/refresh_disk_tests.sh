#!/bin/bash
# Bring the regression programs on an EXISTING data disk up to date without
# touching anything else on it: rewrites /opt/tests in place (and seeds /hello.txt if missing).
# (build_x86_disk.sh makes a fresh image; this keeps the user's files.)
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

# CPython: copied in on first use. An existing /python is left alone unless
# REFRESH_PYTHON=1 (`make disk-python`), which rewrites its files in place and
# keeps everything else on the disk. Needs enough free space on the disk.
PYROOT="$(pwd)/build-x86_64/python/rootfs"
if [ -d "$PYROOT/python" ] && { ! isdir python || [ "${REFRESH_PYTHON:-0}" = 1 ]; }; then
    PYCMDS="$(mktemp)"
    trap 'rm -f "$CMDS" "$PYCMDS"' EXIT
    (
        cd "$PYROOT"
        # mkdir only what is missing (see isdir above)
        find python -type d | sort | while read -r d; do isdir "$d" || echo "mkdir $d"; done
        find python -type f | sort | while read -r f; do
            echo "rm $f"
            echo "write $PYROOT/$f $f"
            echo "sif $f mode 0100$(stat -c %a "$f")"
        done
        find python -type l | sort | while read -r l; do
            echo "rm $l"
            echo "symlink $l $(readlink "$l")"
        done
    ) > "$PYCMDS"
    debugfs -w -f "$PYCMDS" "$DISK" >/dev/null 2>&1 || true
fi
