#!/usr/bin/env python3
"""Persistent-storage test.

1. Create a fresh ext2 data disk.
2. Boot, write files to /mnt (a small file, a multi-block binary that needs
   indirect blocks, a nested directory), `sync`, then kill QEMU outright
   (no clean shutdown).
3. Check the image on the host with e2fsck and read a file back with debugfs.
4. Boot again with the same disk and verify the files from inside the guest.
"""
import os, subprocess, sys

DISK = "build-x86_64/disk-persist.img"
SESSION = os.path.join(os.path.dirname(os.path.abspath(__file__)), "x86_serial_session.py")

def guest(*cmds, timeout=90):
    env = dict(os.environ, EXTRON_DISK=DISK)
    r = subprocess.run([sys.executable, SESSION, "-t", str(timeout), *cmds],
                       env=env, capture_output=True, text=True, timeout=timeout + 60)
    return r.stdout

failures = []
def check(what, ok):
    print(f"[persist] {what:<60} {'PASS' if ok else 'FAIL'}")
    if not ok: failures.append(what)

if os.path.exists(DISK): os.remove(DISK)
subprocess.check_call(["tools/build_x86_disk.sh", DISK, "32"])

out = guest("echo persist-marker-4242 > /mnt/marker.txt",
            "cp /bin/busybox /mnt/bb",
            "mkdir /mnt/dir",
            "echo nested-file > /mnt/dir/n.txt",
            "for i in 1 2 3 4 5 6 7 8 9 10 11 12; do cat /bin/busybox; done > /mnt/big",
            "ls -l /mnt/big",
            "/opt/tests/mlibc_disk_test.elf",
            "sync",
            "echo SYNC_DONE")
check("session 1 wrote files and synced", "SYNC_DONE" in out)
check("in-guest disk workout passed", "[disk_test] ALL PASSED" in out)
if "[disk_test] ALL PASSED" not in out:
    print("\n".join(l for l in out.splitlines() if "disk_test" in l and "PASS" not in l.split()[-1:]))

fsck = subprocess.run(["e2fsck", "-fn", DISK], capture_output=True, text=True)
check("host e2fsck: image is clean", fsck.returncode == 0)
if fsck.returncode:
    print(fsck.stdout, fsck.stderr)
dbg = subprocess.run(["debugfs", "-R", "cat /marker.txt", DISK], capture_output=True, text=True)
check("host debugfs reads marker.txt", "persist-marker-4242" in dbg.stdout)
keep = subprocess.run(["debugfs", "-R", "stat /dt/keep", DISK], capture_output=True, text=True)
check("host sees the shrunk /dt/keep (78853 bytes)", "Size: 78853" in keep.stdout)

out = guest("cat /mnt/marker.txt",
            "cat /mnt/dir/n.txt",
            "cmp /bin/busybox /mnt/bb && echo CMP_OK",
            "for i in 1 2 3 4 5 6 7 8 9 10 11 12; do cat /bin/busybox; done | cmp - /mnt/big && echo BIG_OK",
            "echo VERIFY_DONE")
check("marker.txt survives reboot", "persist-marker-4242" in out)
check("nested file survives reboot", "nested-file" in out)
check("multi-block binary identical after reboot", "CMP_OK" in out)
check("1.4 MB file (double-indirect) identical after reboot", "BIG_OK" in out)

print("[persist]", "FAILED" if failures else "ALL PASSED")
sys.exit(1 if failures else 0)
