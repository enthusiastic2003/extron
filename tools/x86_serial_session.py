#!/usr/bin/env python3
"""Boot build-x86_64/esp headless and drive the guest shell over the serial
console: feeds each command line, waits for the prompt, prints the transcript.
Usage: x86_serial_session.py [-t seconds] 'cmd1' 'cmd2' ..."""
import os, subprocess, sys, tempfile, time, select, fcntl

args = sys.argv[1:]; limit = 40
if args[:1] == ["-t"]: limit = float(args[1]); args = args[2:]
tmp = tempfile.mkdtemp(); vars_ = os.path.join(tmp, "vars.fd")
open(vars_, "wb").write(open("/usr/share/OVMF/OVMF_VARS_4M.fd", "rb").read())
# EXTRON_DISK=<raw image>: attach it as a legacy virtio-blk data disk (/mnt).
disk_args = []
if os.environ.get("EXTRON_DISK"):
    disk_args = ["-drive", f"if=none,id=data0,format=raw,file={os.environ['EXTRON_DISK']}",
                 "-device", "virtio-blk-pci,drive=data0,disable-modern=on"]
q = subprocess.Popen(["qemu-system-x86_64", "-machine", "q35", "-m", "512M", "-cpu", "max", "-display", "none",
    "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-drive", f"if=pflash,format=raw,unit=1,file={vars_}",
    "-drive", "format=raw,file=fat:rw:build-x86_64/esp"] + disk_args + ["-serial", "stdio", "-monitor", "none", "-no-reboot"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
fcntl.fcntl(q.stdout, fcntl.F_SETFL, os.O_NONBLOCK)
buf = b""; t0 = time.time()
def pump(wait_for, timeout, mark=0):
    global buf
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([q.stdout], [], [], 0.1)
        if r:
            try: d = os.read(q.stdout.fileno(), 65536)
            except BlockingIOError: d = b""
            if not d: return False
            buf += d
            if wait_for in buf[mark:][-200:] and len(buf) > mark + 2: return True
    return False
def pump_any(waits, timeout, mark=0):
    """Wait until any of `waits` shows up in the output after `mark`."""
    global buf
    end = time.time() + timeout
    while time.time() < end:
        tail = buf[mark:][-300:]
        for w in waits:
            if w in tail and len(buf) > mark + 2: return w
        r, _, _ = select.select([q.stdout], [], [], 0.1)
        if r:
            try: d = os.read(q.stdout.fileno(), 65536)
            except BlockingIOError: d = b""
            if not d: return None
            buf += d
    return None

# Boot to a shell prompt: either straight into a shell (smoke/AUTOEXIT image)
# or through init -> getty -> login, signing in as root and moving to /.
first = pump_any([b"/ # ", b"login: "], limit)
ok = first is not None
if first == b"login: ":
    mark = len(buf)
    q.stdin.write(b"root\n"); q.stdin.flush()
    pump_any([b"# "], 30, mark)
    mark = len(buf)
    q.stdin.write(b"cd /\n"); q.stdin.flush()
    ok = pump_any([b"/ # "], 15, mark) is not None
for c in args:
    mark = len(buf)
    data = c.encode().replace(b"^C", b"\x03") + b"\n"
    if os.environ.get("PACE"):
        for b in data:
            q.stdin.write(bytes([b])); q.stdin.flush(); time.sleep(float(os.environ["PACE"]))
    else:
        q.stdin.write(data); q.stdin.flush()
    pump(b"/ # " if "^C" not in c else b"#", float(os.environ.get("WAIT", "15")), mark + len(c) + 2)
    time.sleep(0.2)
q.kill()
out = buf.decode(errors="replace").replace("\r", "")
i = out.find("BusyBox")
print(out[i-1:] if i >= 0 else out[-3000:])
