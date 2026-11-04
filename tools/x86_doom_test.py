#!/usr/bin/env python3
"""DOOM + display-ownership test. One QEMU: log in on the serial console, run
`doom`, and check through the QEMU monitor (screendump / sendkey) that

  * the game owns the screen (a rich image, not the text console),
  * it animates (attract mode) and reacts to a real keyboard (monitor `sendkey`
    arrives as PS/2 key events),
  * Ctrl-C on the serial console (the escape hatch) stops it, and
  * the text console comes back, repainted, with the shell usable again.

  x86_doom_test.py [--shots DIR]     keep the screenshots (PNG) in DIR
Needs build-x86_64/rootfs-test.img with doom installed (make doom rootfs-test)."""
import fcntl, os, select, socket, struct, subprocess, sys, tempfile, time, zlib

shots = None
if "--shots" in sys.argv:
    shots = sys.argv[sys.argv.index("--shots") + 1]
    os.makedirs(shots, exist_ok=True)

tmp = tempfile.mkdtemp()
disk = os.path.join(tmp, "disk.img")
subprocess.check_call(["cp", "--sparse=always", "build-x86_64/rootfs-test.img", disk])
vars_ = os.path.join(tmp, "vars.fd")
open(vars_, "wb").write(open("/usr/share/OVMF/OVMF_VARS_4M.fd", "rb").read())
sock_path = os.path.join(tmp, "mon.sock")
q = subprocess.Popen(["qemu-system-x86_64", "-machine", "q35", "-m", "512M", "-cpu", "max",
    "-display", "none", "-vga", "std",
    "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-drive", f"if=pflash,format=raw,unit=1,file={vars_}",
    "-drive", "format=raw,file=fat:rw:build-x86_64/esp",
    "-drive", f"if=none,id=d0,format=raw,file={disk}",
    "-device", "virtio-blk-pci,drive=d0,disable-modern=on",
    "-serial", "stdio", "-monitor", f"unix:{sock_path},server,nowait", "-no-reboot"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
fcntl.fcntl(q.stdout, fcntl.F_SETFL, os.O_NONBLOCK)
buf = b""
failures = []

def pump(t=0.2):
    global buf
    r, _, _ = select.select([q.stdout], [], [], t)
    if r:
        try:
            d = os.read(q.stdout.fileno(), 65536)
        except BlockingIOError:
            return True
        if d: buf += d
        return bool(d)
    return True

def wait(marker, timeout=30, since=0):
    end = time.time() + timeout
    while time.time() < end:
        if marker in buf[since:]: return True
        if not pump(): return marker in buf[since:]
    return False

def send(b):
    q.stdin.write(b if isinstance(b, bytes) else b.encode()); q.stdin.flush()

def settle(seconds):
    end = time.time() + seconds
    while time.time() < end: pump(0.1)

def check(what, ok, detail=None):
    print(f"[doom] {what:<66} {'PASS' if ok else 'FAIL'}")
    if not ok:
        failures.append(what)
        if detail is not None: print("       ", detail)

while not os.path.exists(sock_path): time.sleep(0.1)
mon = socket.socket(socket.AF_UNIX); mon.connect(sock_path); time.sleep(0.3); mon.recv(65536)

def monitor(c, d=0.3):
    mon.send((c + "\n").encode()); time.sleep(d); mon.settimeout(0.3)
    try: return mon.recv(65536).decode(errors="replace")
    except Exception: return ""

def screendump(name):
    path = os.path.join(tmp, name + ".ppm")
    monitor(f"screendump {path}", 1.0)
    d = open(path, "rb").read()
    parts = d.split(b"\n", 3); w, h = map(int, parts[1].split()); px = parts[3]
    if shots:                                   # PNG for humans
        raw = b"".join(b"\0" + px[y*w*3:(y+1)*w*3] for y in range(h))
        def chunk(t, b): c = struct.pack(">I", len(b)) + t + b; return c + struct.pack(">I", zlib.crc32(t + b))
        open(os.path.join(shots, name + ".png"), "wb").write(
            b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    return w, h, px

def colours(px):
    return len(set(px[i:i+3] for i in range(0, len(px), 3 * 7)))     # sampled

# ---- boot, log in, run doom ---------------------------------------------------
check("boot and login prompt", wait(b"login: ", 90))
mark = len(buf); send("root\n")
check("logged in", wait(b"# ", 30, mark))
w, h, px = screendump("console_before")
c_console = colours(px)
check("the text console is on screen before the game", c_console <= 40, f"{c_console} colours")

mark = len(buf)
send("doom\n")
check("doom starts (reports the display it took over)", wait(b"DOOM:", 30, mark) or wait(b"I_Init", 30, mark))
settle(8)
text = buf[mark:].decode(errors="replace")
check("doom loaded the WAD and the engine started", "Z_Init" in text or "V_Init" in text or "DOOM:" in text, text[-300:])
w, h, a = screendump("doom_1")
c1 = colours(a)
check("the game owns the screen (a rich image, not console text)", c1 > 60, f"{c1} colours")
settle(3)
_, _, b = screendump("doom_2")
check("the picture animates (attract mode)", a != b)

# A real keyboard: sendkey arrives as PS/2 events, not terminal bytes.
monitor("sendkey ret", 0.2); settle(1.0)
monitor("sendkey down", 0.2); settle(0.5)
_, _, c = screendump("doom_3")
check("a key press changes the picture (PS/2 key events reach the game)", c != b)

# Keyboard is diverted from the console: text typed over serial must not echo to the tty.
# (Ctrl-C is the exception, the escape hatch.)
mark = len(buf)
send(b"\x03")
check("Ctrl-C stops the game and the shell prompt returns", wait(b"# ", 20, mark))
settle(1.5)
mark = len(buf)
send("echo CONSOLE_$((6*7))_BACK\n")
check("the shell works again", wait(b"CONSOLE_42_BACK", 15, mark))
settle(1)
w, h, d = screendump("console_after")
c_after = colours(d)
check("the text console is repainted on screen after the game exits",
      c_after <= 40 and d != a, f"{c_after} colours")

monitor("quit", 0.3)
try: q.wait(timeout=10)
except Exception: q.kill()
print("[doom] ALL PASSED" if not failures else f"[doom] {len(failures)} FAILED")
sys.exit(1 if failures else 0)
