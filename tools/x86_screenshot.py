#!/usr/bin/env python3
"""Boot build-x86_64/esp under QEMU+OVMF, wait, screendump the guest display
to a PNG (and optionally type keys via the monitor). Usage:
  x86_screenshot.py out.png [seconds] [sendkey ...]"""
import os, socket, subprocess, sys, time, zlib, struct, tempfile

out = sys.argv[1]; wait = float(sys.argv[2]) if len(sys.argv) > 2 else 6
keys = sys.argv[3:]
tmp = tempfile.mkdtemp()
sock = os.path.join(tmp, "mon.sock")
vars_ = os.path.join(tmp, "vars.fd")
open(vars_, "wb").write(open("/usr/share/OVMF/OVMF_VARS_4M.fd", "rb").read())
q = subprocess.Popen(["qemu-system-x86_64", "-machine", "q35", "-m", "512M", "-cpu", "max",
    "-display", "none", "-vga", "std",
    "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-drive", f"if=pflash,format=raw,unit=1,file={vars_}",
    "-drive", "format=raw,file=fat:rw:build-x86_64/esp",
    "-serial", f"file:{tmp}/serial.log", "-monitor", f"unix:{sock},server,nowait", "-no-reboot"],
    stderr=subprocess.DEVNULL)
while not os.path.exists(sock): time.sleep(0.1)
m = socket.socket(socket.AF_UNIX); m.connect(sock); time.sleep(0.3); m.recv(65536)
def cmd(c, d=0.2):
    m.send((c + "\n").encode()); time.sleep(d); m.settimeout(0.3)
    try: m.recv(65536)
    except Exception: pass
time.sleep(wait)
for k in keys:
    cmd("sendkey " + k, 0.15)
if keys: time.sleep(1.0)
ppm = os.path.join(tmp, "s.ppm")
cmd(f"screendump {ppm}", 1.0)
cmd("quit", 0.3); q.wait(timeout=10)
d = open(ppm, "rb").read()
parts = d.split(b"\n", 3); w, h = map(int, parts[1].split()); px = parts[3]
raw = b"".join(b"\0" + px[y*w*3:(y+1)*w*3] for y in range(h))
def chunk(t, b): c = struct.pack(">I", len(b)) + t + b; return c + struct.pack(">I", zlib.crc32(t + b))
open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
print("wrote", out, w, h); print(open(f"{tmp}/serial.log", errors="replace").read()[-600:])
