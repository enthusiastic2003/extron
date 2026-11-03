#!/usr/bin/env python3
"""Disk-timeout and RTC write-back test (one QEMU session, ~1 minute).

1. Boot, log in, read a file normally.
2. Through the QEMU monitor throttle the virtio disk to ~1 byte/s so a read
   hangs: the kernel must give up after ~5 s, return EIO to the program, reset
   the device, and keep working once the throttle is lifted.
3. Set the clock (date -s), reboot the VM (QEMU stays up, so its RTC persists)
   and check the new boot's clock started from the value written to the RTC.
4. Power off; the data disk must be consistent afterwards.
"""
import fcntl, os, re, select, socket, subprocess, sys, tempfile, time

tmp = tempfile.mkdtemp()
vars_ = os.path.join(tmp, "vars.fd")
open(vars_, "wb").write(open("/usr/share/OVMF/OVMF_VARS_4M.fd", "rb").read())
disk = os.path.join(tmp, "disk.img")
mon_path = os.path.join(tmp, "mon.sock")
subprocess.check_call(["tools/build_x86_disk.sh", disk, "32"])
# No -no-reboot: the VM reboot in step 3 must not end QEMU.
q = subprocess.Popen(["qemu-system-x86_64", "-machine", "q35", "-m", "512M", "-cpu", "max", "-display", "none",
    "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-drive", f"if=pflash,format=raw,unit=1,file={vars_}",
    "-drive", "format=raw,file=fat:rw:build-x86_64/esp",
    "-drive", f"if=none,id=d0,format=raw,file={disk}",
    "-device", "virtio-blk-pci,drive=d0,disable-modern=on",
    "-serial", "stdio", "-monitor", f"unix:{mon_path},server,nowait"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
fcntl.fcntl(q.stdout, fcntl.F_SETFL, os.O_NONBLOCK)
buf = b""
failures = []

def pump(t=0.2):
    global buf
    r, _, _ = select.select([q.stdout], [], [], t)
    if r:
        d = os.read(q.stdout.fileno(), 65536)
        if d: buf += d
        return bool(d)
    return True

def wait(marker, timeout=30, since=0):
    end = time.time() + timeout
    while time.time() < end:
        if marker in buf[since:]: return True
        if not pump(): return marker in buf[since:]
    return marker in buf[since:]

def send(line): q.stdin.write(line.encode() + b"\n"); q.stdin.flush()

def monitor(cmd):
    for _ in range(50):
        try:
            s = socket.socket(socket.AF_UNIX); s.connect(mon_path); break
        except OSError: time.sleep(0.1)
    s.settimeout(3)
    try: s.recv(4096)
    except OSError: pass
    s.sendall(cmd.encode() + b"\n"); time.sleep(0.3)
    try: out = s.recv(4096).decode(errors="replace")
    except OSError: out = ""
    s.close()
    return out

def check(what, ok, detail=None):
    print(f"[disk] {what:<64} {'PASS' if ok else 'FAIL'}")
    if not ok:
        failures.append(what)
        if detail: print("       detail:", repr(detail.strip()[-500:]))

def run(cmd, timeout=30):
    mark = len(buf)
    send(f"{cmd}; echo @@EN$((0))D@@")
    wait(b"@@EN0D@@", timeout, mark)
    return buf[mark:].decode(errors="replace").replace("\r", "")

def login():
    mark = len(buf)
    if not wait(b"login: ", 90, mark): return False
    send("root")
    return wait(b"# ", 30, mark)

check("boot and log in", login())
out = run("cat /mnt/opt/tests/mlibc_file_test.elf > /dev/null; echo rc=$?")
check("a cold read works normally", "rc=0" in out, out)

# --- hung disk ---------------------------------------------------------------
mon = monitor("block_set_io_throttle d0 1 0 0 0 0 0")
check("monitor accepted the throttle", "rror" not in mon, mon)
mark = len(buf)
t0 = time.time()
out = run("cat /mnt/opt/tests/mlibc_dynamic_test.elf > /dev/null; echo rc=$?", 60)
dt = time.time() - t0
text = buf[mark:].decode(errors="replace")
check("the read fails instead of hanging forever", "rc=1" in out or "rc=0" not in out and "rc=" in out, out)
check("...with an I/O error", "Input/output error" in out or "EIO" in out, out)
check("...after about the 5 s timeout", 4.0 <= dt <= 25.0, f"{dt:.1f}s\n{text}")
check("the kernel logged the timeout and the reset",
      "request timed out" in text and "device reset" in text, text)

monitor("block_set_io_throttle d0 0 0 0 0 0 0")
time.sleep(1)
out = run("cat /mnt/opt/tests/hello.txt; echo rc=$?", 30)
check("the disk works again after the reset", "rc=0" in out and "hello" in out.lower(), out)
out = run("echo post-reset > /mnt/after-reset.txt; sync; cat /mnt/after-reset.txt", 30)
check("writes work again too", "post-reset" in out, out)

# --- RTC write-back ------------------------------------------------------------
out = run("date -u -s '2030-01-02 03:04:05'; sync")
mark = len(buf)
send("reboot")
check("the VM reboots and a login prompt returns", wait(b"login: ", 120, mark))
mark = len(buf)                      # after the prompt: ignore the old shell's "# "
send("root"); wait(b"# ", 30, mark)
time.sleep(1)
out = run("date -u")
check("the new boot's clock came from the RTC we wrote (2030-01-02)",
      re.search(r"Jan\s+2 0[34]:\d\d:\d\d UTC 2030", out) is not None, out)

# --- shutdown ---------------------------------------------------------------
send("poweroff")
t0 = time.time()
while q.poll() is None and time.time() - t0 < 30:
    pump(0.2)
check("poweroff ends QEMU", q.poll() is not None)
if q.poll() is None: q.kill()
fsck = subprocess.run(["e2fsck", "-fn", disk], capture_output=True, text=True)
check("host e2fsck: the disk is clean", fsck.returncode == 0, fsck.stdout)
data = subprocess.run(["debugfs", "-R", "cat /after-reset.txt", disk], capture_output=True, text=True).stdout
check("data written after the reset reached the disk", "post-reset" in data, data)
print("[disk] ALL PASSED" if not failures else f"[disk] {len(failures)} FAILED")
sys.exit(1 if failures else 0)
