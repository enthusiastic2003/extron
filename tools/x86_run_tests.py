#!/usr/bin/env python3
"""Boot build-x86_64/esp under QEMU+OVMF (BusyBox image) and run the mlibc
regression suite (`sh /bin/runtests`) N times back to back, failing on any test
failure, nonzero exit, kernel panic/CPU exception, hang, or leaked memory.

  x86_run_tests.py [-n ITERATIONS] [-t TIMEOUT_PER_ITERATION] [--log FILE] [--cmd CMD]
Exit status 0 only if every iteration was clean."""
import argparse, os, re, select, subprocess, sys, tempfile, time, fcntl

ap = argparse.ArgumentParser()
ap.add_argument("-n", type=int, default=1)
ap.add_argument("-t", type=float, default=240)
ap.add_argument("--log")
ap.add_argument("--cmd", default="sh /bin/runtests")
ap.add_argument("--done", default="RUNTESTS DONE")
ap.add_argument("--mem", default="512M")
args = ap.parse_args()

tmp = tempfile.mkdtemp()
vars_ = os.path.join(tmp, "vars.fd")
open(vars_, "wb").write(open("/usr/share/OVMF/OVMF_VARS_4M.fd", "rb").read())
# Fresh data disk per run so tests that use /mnt (virtio-blk + ext2) run too.
disk_img = os.path.join(tmp, "disk.img")
disk_args = []
try:
    subprocess.check_call(["tools/build_x86_disk.sh", disk_img, "64"])
    disk_args = ["-drive", f"if=none,id=data0,format=raw,file={disk_img}",
                 "-device", "virtio-blk-pci,drive=data0,disable-modern=on"]
except (OSError, subprocess.CalledProcessError):
    print("note: mke2fs unavailable; running without a data disk")
q = subprocess.Popen(["qemu-system-x86_64", "-machine", "q35", "-m", args.mem, "-cpu", "max", "-display", "none",
    "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-drive", f"if=pflash,format=raw,unit=1,file={vars_}",
    "-drive", "format=raw,file=fat:rw:build-x86_64/esp"] + disk_args + ["-serial", "stdio", "-monitor", "none", "-no-reboot"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
fcntl.fcntl(q.stdout, fcntl.F_SETFL, os.O_NONBLOCK)
buf = b""

def read_until(marker, timeout, start=0):
    global buf
    end = time.time() + timeout
    while time.time() < end:
        if marker in buf[start:]:
            return True
        r, _, _ = select.select([q.stdout], [], [], 0.2)
        if r:
            d = os.read(q.stdout.fileno(), 65536)
            if not d:
                return False
            buf += d
        if q.poll() is not None and not select.select([q.stdout], [], [], 0)[0]:
            return marker in buf[start:]
    return False

def send(line):
    for b in line.encode() + b"\n":
        q.stdin.write(bytes([b])); q.stdin.flush(); time.sleep(0.002)

def memfree():
    """Free physical pages as reported by the kernel (sysconf), or None."""
    global buf
    mark = len(buf)
    send("memfree")
    if not read_until(b"/ # ", 20, mark + 1): return None
    m = re.search(rb"MEMFREE (\d+)", buf[mark:])
    return int(m.group(1)) if m else None

LEAK_TOLERANCE = 48   # pages; the page cache and ext2 metadata may legitimately grow a little
problems = []
baseline = None
if not read_until(b"/ # ", 90):
    problems.append("never reached the shell prompt")
else:
    baseline = memfree()
    print("free pages before: %s" % baseline, flush=True)
    for it in range(1, args.n + 1):
        mark = len(buf)
        t0 = time.time()
        send(args.cmd)
        ok = read_until(args.done.encode(), args.t, mark)
        text = buf[mark:].decode(errors="replace").replace("\r", "")
        dt = time.time() - t0
        bad = []
        if not ok: bad.append("timeout/hang (no '%s')" % args.done)
        for pat, what in ((r"\] [^\n]*\bFAIL\b", "test FAIL"), (r"\[exit status", "nonzero exit"),
                          (r"KERNEL PANIC", "kernel panic"), (r"CPU EXCEPTION", "CPU exception")):
            m = re.findall(pat, text)
            if m: bad.append(f"{len(m)}x {what}")
        read_until(b"/ # ", 20, mark + 1)
        now = memfree() if ok else None
        if baseline is not None and now is not None and baseline - now > LEAK_TOLERANCE:
            bad.append(f"memory leak: {baseline - now} pages lost since boot")
        print(f"iteration {it}/{args.n}: {'OK' if not bad else 'PROBLEM: ' + '; '.join(bad)}  ({dt:.0f}s, free pages {now})", flush=True)
        if bad:
            problems.append(f"iteration {it}: " + "; ".join(bad))
            for line in text.splitlines():
                if re.search(r"FAIL|exit status|PANIC|EXCEPTION", line): print("   ", line[:140])
            if not ok: break

q.kill()
if args.log:
    open(args.log, "w").write(buf.decode(errors="replace"))
if problems:
    print("RESULT: FAIL"); [print(" -", p) for p in problems]; sys.exit(1)
print("RESULT: PASS (%d iteration%s)" % (args.n, "" if args.n == 1 else "s"))
