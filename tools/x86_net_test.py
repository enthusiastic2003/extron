#!/usr/bin/env python3
"""Networking test over QEMU's user-mode network. One QEMU, one serial login.

  * DHCP gave the guest an address, gateway and DNS server (/proc/net/ipconfig,
    /etc/resolv.conf -> /proc/net/resolv.conf),
  * BusyBox wget, nc and Python's urllib fetch from an HTTP server running on the
    host (reached as 10.0.2.2), including a multi-megabyte file checked by SHA-256,
  * the guest can listen: the host connects to a guest port through QEMU's
    host-forwarding and the bytes arrive,
  * name resolution and HTTPS work when the host has internet (skipped otherwise).

Needs build-x86_64/rootfs-test.img (make rootfs-test)."""
import fcntl, hashlib, http.server, os, random, select, socket, subprocess, sys
import tempfile, threading, time

tmp = tempfile.mkdtemp()
web = os.path.join(tmp, "web")
os.makedirs(web)
HELLO = b"hello from the host, through lwIP\n"
open(os.path.join(web, "hello.txt"), "wb").write(HELLO)
BIG = random.Random(7).randbytes(6 * 1024 * 1024)
open(os.path.join(web, "big.bin"), "wb").write(BIG)
BIG_SHA = hashlib.sha256(BIG).hexdigest()

class Quiet(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k): super().__init__(*a, directory=web, **k)
    def log_message(self, *a): pass

httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
HTTP_PORT = httpd.server_address[1]
threading.Thread(target=httpd.serve_forever, daemon=True).start()

# A server that trickles its body in small pieces with pauses, like a distant internet host:
# the guest then reads many short chunks instead of a few big ones (this once exposed
# data loss in stdio over sockets).
TRICKLE = random.Random(11).randbytes(2 * 1024 * 1024)
TRICKLE_SHA = hashlib.sha256(TRICKLE).hexdigest()
class Trickle(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def log_message(self, *a): pass
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Length", str(len(TRICKLE)))
        self.end_headers()
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        for i in range(0, len(TRICKLE), 1440):
            self.wfile.write(TRICKLE[i:i + 1440]); self.wfile.flush()
            if (i // 1440) % 4 == 0: time.sleep(0.0004)
trickled = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Trickle)
TRICKLE_PORT = trickled.server_address[1]
threading.Thread(target=trickled.serve_forever, daemon=True).start()

# The same files over HTTPS with a throwaway RSA certificate.
import ssl
key, crt = os.path.join(tmp, "key.pem"), os.path.join(tmp, "crt.pem")
subprocess.check_call(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key,
                       "-out", crt, "-days", "2", "-subj", "/CN=extron-test"],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
tls.minimum_version = tls.maximum_version = ssl.TLSVersion.TLSv1_2
tls.set_ciphers("ECDHE-RSA-AES128-GCM-SHA256")
tls.load_cert_chain(crt, key)
httpsd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
httpsd.socket = tls.wrap_socket(httpsd.socket, server_side=True)
HTTPS_PORT = httpsd.server_address[1]
threading.Thread(target=httpsd.serve_forever, daemon=True).start()

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p
FWD_PORT = free_port()

def internet():
    try:
        socket.create_connection(("example.com", 80), timeout=3).close()
        return True
    except OSError:
        return False
HAVE_NET = internet()

disk = os.path.join(tmp, "disk.img")
subprocess.check_call(["cp", "--sparse=always", "build-x86_64/rootfs-test.img", disk])
vars_ = os.path.join(tmp, "vars.fd")
open(vars_, "wb").write(open("/usr/share/OVMF/OVMF_VARS_4M.fd", "rb").read())
q = subprocess.Popen(["qemu-system-x86_64", "-machine", "q35", "-m", "512M", "-cpu", "max", "-display", "none",
    "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-drive", f"if=pflash,format=raw,unit=1,file={vars_}",
    "-drive", "format=raw,file=fat:rw:build-x86_64/esp",
    "-drive", f"if=none,id=d0,format=raw,file={disk}",
    "-device", "virtio-blk-pci,drive=d0,disable-modern=on",
    "-netdev", f"user,id=n0,hostfwd=tcp:127.0.0.1:{FWD_PORT}-:8080",
    "-device", "virtio-net-pci,netdev=n0,disable-modern=on",
    "-serial", "stdio", "-monitor", "none", "-no-reboot"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
fcntl.fcntl(q.stdout, fcntl.F_SETFL, os.O_NONBLOCK)
buf = b""
failures = []

def pump(t=0.2):
    global buf
    r, _, _ = select.select([q.stdout], [], [], t)
    if r:
        try: d = os.read(q.stdout.fileno(), 65536)
        except BlockingIOError: return True
        if d: buf += d
        return bool(d)
    return True

def wait(marker, timeout=30, since=0):
    end = time.time() + timeout
    while time.time() < end:
        if marker in buf[since:]: return True
        if not pump(): return marker in buf[since:]
    return False

def run(cmd, timeout=60):
    """Run a command and return its output. The end marker is built by the shell so
    the echoed command line cannot match it."""
    mark = len(buf)
    q.stdin.write(f"{cmd}; echo @@EN$((0))D@@\n".encode()); q.stdin.flush()
    wait(b"@@EN0D@@", timeout, mark)
    return buf[mark:].decode(errors="replace").replace("\r", "")

def check(what, ok, detail=None):
    print(f"[net] {what:<66} {'PASS' if ok else 'FAIL'}")
    if not ok:
        failures.append(what)
        if detail is not None: print("      ", detail.strip()[-500:])

def skip(what):
    print(f"[net] {what:<66} SKIP (no internet on the host)")

check("boot and login prompt", wait(b"login: ", 90))
mark = len(buf); q.stdin.write(b"root\n"); q.stdin.flush()
check("logged in", wait(b"# ", 30, mark))
run("cd /")

# ---- DHCP ----------------------------------------------------------------------
out = ""
for _ in range(20):
    out = run("cat /proc/net/ipconfig")
    if "ip=10.0.2.15" in out: break
    time.sleep(0.5)
check("DHCP: the guest got 10.0.2.15/24 with gateway 10.0.2.2",
      "ip=10.0.2.15" in out and "mask=255.255.255.0" in out and "gw=10.0.2.2" in out, out)
check("DHCP: the DNS server is 10.0.2.3", "dns=10.0.2.3" in out, out)
out = run("cat /etc/resolv.conf")
check("/etc/resolv.conf shows the DHCP nameserver", "nameserver 10.0.2.3" in out, out)

# ---- downloads from the host --------------------------------------------------------
out = run(f"wget -q -O - http://10.0.2.2:{HTTP_PORT}/hello.txt")
check("wget: a small file from the host's HTTP server", "hello from the host, through lwIP" in out, out)
t0 = time.time()
run(f"wget -q -O /tmp/big http://10.0.2.2:{HTTP_PORT}/big.bin", 120)
dt = time.time() - t0
out = run("sha256sum /tmp/big; ls -l /tmp/big")
check(f"wget: a 6 MiB file arrives intact (SHA-256) in {dt:.1f}s ({6/dt:.1f} MiB/s)",
      BIG_SHA in out, out)
out = run(f"python3 -c \"import urllib.request as u; print(u.urlopen('http://10.0.2.2:{HTTP_PORT}/hello.txt').read().decode().strip())\"", 60)
check("Python urllib: the same file over its own socket code",
      "hello from the host, through lwIP" in out, out)
out = run(f"printf 'GET /hello.txt HTTP/1.0\\r\\n\\r\\n' | nc 10.0.2.2 {HTTP_PORT}")
check("nc: a hand-written HTTP request", "200 OK" in out and "through lwIP" in out, out)
for line in ("import socket",
             f"s = socket.create_connection(('10.0.2.2', {HTTP_PORT}), 5)",
             "s.sendall(b'GET /big.bin HTTP/1.0\\r\\n\\r\\n')",
             "n = 0",
             "while True:",
             "    d = s.recv(65536)",
             "    if not d: break",
             "    n += len(d)",
             "print('RECV', n)"):
    run(f"echo \"{line}\" >> /tmp/recv.py")
out = run("python3 /tmp/recv.py", 120)
check("Python socket: a 6 MiB body streamed through recv()",
      "RECV 6" in out and abs(int(out.split("RECV")[1].split()[0]) - len(BIG)) < 400, out)

out = run(f"wget -q -O /tmp/trickle http://10.0.2.2:{TRICKLE_PORT}/t 2>&1; sha256sum /tmp/trickle", 120)
check("wget: a body that arrives in many short chunks is intact (SHA-256)", TRICKLE_SHA in out, out)

# ---- the guest as a server ---------------------------------------------------------------
mark = len(buf)
q.stdin.write(b"(nc -l -p 8080 > /tmp/rx; echo @@LISTENER_$((6*7))@@) &\n"); q.stdin.flush()
time.sleep(1.0)
try:
    c = socket.create_connection(("127.0.0.1", FWD_PORT), timeout=10)
    c.sendall(b"hello-from-the-host-to-the-guest\n")
    time.sleep(0.5)
    c.close()
except OSError as e:
    print("      host connect failed:", e)
ok = wait(b"@@LISTENER_42@@", 20, mark)
out = run("cat /tmp/rx")
check("the guest accepts a connection (host -> guest via port forward)",
      ok and "hello-from-the-host-to-the-guest" in out, out)

# ---- ICMP, DNS, TLS -------------------------------------------------------------------------
out = run("ping -c 2 -W 3 10.0.2.2", 30)
check("ping: ICMP echo to the gateway (raw ICMP socket)", "2 packets received" in out or "0% packet loss" in out, out)
out = run(f"wget -q -O - https://10.0.2.2:{HTTPS_PORT}/hello.txt 2>/dev/null", 60)
check("wget over HTTPS (TLS 1.2, RSA certificate) from the host", "hello from the host, through lwIP" in out, out)
out = run(f"wget -q -O /tmp/bigtls https://10.0.2.2:{HTTPS_PORT}/big.bin 2>/dev/null; sha256sum /tmp/bigtls", 180)
check("a 6 MiB file over TLS arrives intact (SHA-256)", BIG_SHA in out, out)
if HAVE_NET:
    out = run("nslookup example.com 10.0.2.3 2>&1", 40)
    check("nslookup resolves example.com (DNS over UDP through the NAT)",
          "Name:" in out and "Address 1:" in out, out)
    out = run("wget -q -O - http://example.com 2>&1 | head -5", 60)
    check("wget over plain HTTP to a real website, by name", "Example Domain" in out, out)
    out = run("wget -q -O - https://example.com 2>&1 | head -3", 90)
    check("wget over HTTPS to a public site", "Example Domain" in out, out)
    # Real source tarballs, the point of all this: compare with the host's own download.
    import urllib.request
    want = hashlib.sha256(urllib.request.urlopen(
        "http://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz", timeout=60).read()).hexdigest()
    out = run("wget -q -O /tmp/make.tgz http://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz; "
              "sha256sum /tmp/make.tgz", 180)
    check("GNU make's source tarball downloaded over HTTP matches the host's (SHA-256)", want in out, out)
    out = run("wget -q -O /tmp/zlib.tgz https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz; "
              "tar tzf /tmp/zlib.tgz | head -2", 180)
    check("zlib's source from GitHub (HTTPS, redirect) unpacks with tar",
          "zlib-1.3.1/" in out and "inflate.c" not in out, out)
    out = run("cd /tmp && tar xzf zlib.tgz && ls zlib-1.3.1/inflate.c zlib-1.3.1/configure", 60)
    check("...and the extracted tree has its files", "inflate.c" in out and "configure" in out, out)
else:
    print("[net] DNS and public-website checks                                SKIP (no internet on the host)")

run("poweroff -f", 5)
try: q.wait(timeout=15)
except Exception: q.kill()
httpd.shutdown()
trickled.shutdown()
httpsd.shutdown()
print("[net] ALL PASSED" if not failures else f"[net] {len(failures)} FAILED")
sys.exit(1 if failures else 0)
