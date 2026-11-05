#!/usr/bin/env python3
"""Boot-flow test: init -> getty -> login -> shell, accounts, permissions,
respawn and shutdown. Runs one QEMU and drives the serial console.

  x86_boot_test.py              boot-flow checks only
  x86_boot_test.py --runtests   ...and the whole regression suite (`sh /bin/runtests`)
                                in the same boot, before powering off
Exit status 0 only if every check passes."""
import fcntl, os, re, select, subprocess, sys, tempfile, time

RUN_TESTS = "--runtests" in sys.argv

tmp = tempfile.mkdtemp()
vars_ = os.path.join(tmp, "vars.fd")
open(vars_, "wb").write(open("/usr/share/OVMF/OVMF_VARS_4M.fd", "rb").read())
disk = os.path.join(tmp, "disk.img")
subprocess.check_call(["cp", "--sparse=always", "build-x86_64/rootfs-test.img", disk])
q = subprocess.Popen(["qemu-system-x86_64", "-machine", "q35", "-m", "512M", "-cpu", "max", "-display", "none",
    "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-drive", f"if=pflash,format=raw,unit=1,file={vars_}",
    "-drive", "format=raw,file=fat:rw:build-x86_64/esp",
    "-drive", f"if=none,id=d0,format=raw,file={disk}",
    "-device", "virtio-blk-pci,drive=d0,disable-modern=on",
    "-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0,disable-modern=on",
    "-serial", "stdio", "-monitor", "none", "-no-reboot"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
fcntl.fcntl(q.stdout, fcntl.F_SETFL, os.O_NONBLOCK)
buf = b""
failures = []

def pump(timeout=0.2):
    global buf
    r, _, _ = select.select([q.stdout], [], [], timeout)
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
    return False

def send(line):
    q.stdin.write(line.encode() + b"\n"); q.stdin.flush()

def check(what, ok, detail=None):
    print(f"[boot] {what:<62} {'PASS' if ok else 'FAIL'}")
    if not ok:
        failures.append(what)
        if detail is not None:
            print("       output was:", repr(detail.strip()[-600:]))

def run(cmd):
    """Run a command and return its output. The end marker is built by the
    shell ($((0))) so the echoed command line itself cannot match it."""
    mark = len(buf)
    send(f"{cmd}; echo @@EN$((0))D@@")
    wait(b"@@EN0D@@", 20, mark)
    text = buf[mark:].decode(errors="replace").replace("\r", "")
    if os.environ.get("BOOT_DEBUG"):
        print(f"--- run({cmd!r}) ->\n{text}\n---", flush=True)
    return text

def login(user, password=None, have_prompt=False):
    mark = len(buf)
    if not have_prompt:
        check(f"login prompt for {user}", wait(b"login: ", 60, mark))
    send(user)
    if password is not None:
        wait(b"Password: ", 10, mark)
        send(password)
    return wait(b"# " if user == "root" else b"$ ", 15, mark)

# --- first login: root has no password -------------------------------------
check("kernel started init and getty printed a login prompt", wait(b"login: ", 90))
check("root logs in without a password", login("root", have_prompt=True))
out = run("id; pwd; hostname; echo $HOME")
check("root shell has uid 0", "uid=0(root)" in out)
check("root's home is /root", "/root\n" in out)
check("rcS set the hostname from /etc/hostname", "extron" in out)
out = run("cat /etc/motd")
check("/etc/motd is shown", "hobby POSIX kernel" in out)
out = run("ls -l /etc/shadow; ps >/dev/null 2>&1; echo rc=$?")
check("/etc/shadow is root-only (0600)", "-rw-------" in out)
out = run("printf '#!/bin/sh\\necho script-ran $1 $2\\n' > /tmp/s.sh; chmod +x /tmp/s.sh; /tmp/s.sh a b")
check("execve runs #! scripts directly", "script-ran a b" in out)
out = run("for a in sync init getty login poweroff gzip ls; do p=$(command -v $a) || echo MISS\\ING-$a; done; test -x /bin/sync && echo sync-$((0))k")
check("every applet init/inittab needs exists (e.g. /bin/sync)", "MISSING" not in out and "sync-0k" in out, out)
out = run("cat /var/log/messages")
check("init logs to /var/log/messages, not the console", "init started" in out)
out = run("ps; free; uptime; echo pid1=$(pidof init)")
check("ps lists init and the shell via /proc", "/sbin/init" in out and ("-sh" in out or "-bash" in out), out)
check("free reports memory (sysinfo)", "Mem:" in out and "Swap:" in out, out)
check("uptime and pidof work", "load average" in out and "pid1=1" in out, out)
out = run("sleep 100 & P=$!; sleep 1; killall sleep; wait $P; echo st=$?")
check("killall finds processes through /proc (child died of SIGTERM)", "st=143" in out, out)
out = run("/bin/echo $(seq 1 900) | wc -w; /bin/echo $(seq 1 900) | md5sum; echo $(seq 1 900) | md5sum")
hashes = re.findall(r"^([0-9a-f]{32})  -$", out, re.M)
check("execve carries ~3.4 KB of arguments intact",
      re.search(r"^900$", out, re.M) is not None and len(hashes) == 2 and hashes[0] == hashes[1], out)
out = run("/bin/echo $(seq 1 3000) | wc -w")
check("too many arguments fail with E2BIG, not EPERM", "Argument list too long" in out, out)
out = run('A=$(seq 1 1500 | tr -d "\\n"); export A; /bin/sh -c "echo \\${#A}"')
check("a ~5 KB environment variable survives exec", "4893" in out, out)
out = run("sleep 1 & wait; echo bg-ok")
check("job control works under init", "bg-ok" in out)
out = run("mkfifo /tmp/f; (echo via-fifo > /tmp/f &) ; cat /tmp/f")
check("a FIFO works from the login shell", "via-fifo" in out)

# --- logout: getty must respawn --------------------------------------------
mark = len(buf)
send("exit")
check("logging out brings the login prompt back (respawn)", wait(b"login: ", 30, mark))

# --- wrong password, then a normal user ------------------------------------
mark = len(buf)
send("user"); wait(b"Password: ", 10, mark); send("not-the-password")
check("a wrong password is rejected", wait(b"Login incorrect", 15, mark))
check("...and the prompt returns", wait(b"login: ", 15, mark))
check("user logs in with the right password", login("user", "user", have_prompt=True))
out = run("id; pwd")
check("user shell has uid 1000 in /home/user", "uid=1000(user)" in out and "/home/user" in out)
out = run("cat /etc/shadow; echo rc=$?")
check("a normal user cannot read /etc/shadow", "Access denied" in out or "rc=1" in out)
out = run("echo x > /etc/forbidden; echo rc=$?")
check("a normal user cannot write /etc", "rc=0" not in out)
out = run("echo mine > ~/note; cat ~/note")
check("a normal user can write its home", "mine" in out)
mark = len(buf)
send("exit")
check("login prompt after the user logs out", wait(b"login: ", 30, mark))

# --- shutdown: data written before poweroff must be on disk -----------------
check("root logs in again", login("root", have_prompt=True))

if RUN_TESTS:
    # The whole regression suite, in this same boot (init + getty + login are
    # up and the root disk is mounted).
    run("cd /")
    before = run("memfree")
    mark = len(buf)
    send("sh /bin/runtests")
    done = wait(b"RUNTESTS DONE", int(os.environ.get("SUITE_TIMEOUT", "900")), mark)
    text = buf[mark:].decode(errors="replace").replace("\r", "")
    check("regression suite finished (no hang)", done,
          None if done else "stalled after: " + text.strip()[-1200:])
    fails = re.findall(r"\] [^\n]*\bFAIL\b[^\n]*", text)
    check("no regression test reported FAIL", not fails, "\n".join(fails[:10]))
    check("no test exited non-zero", "[exit status" not in text,
          "\n".join(l for l in text.splitlines() if "[exit status" in l))
    check("no kernel panic / CPU exception",
          "KERNEL PANIC" not in text and "CPU EXCEPTION" not in text)
    after = run("memfree")
    m1, m2 = re.search(r"MEMFREE (\d+)", before), re.search(r"MEMFREE (\d+)", after)
    # Page cache and heap high-water legitimately grow once; flag only a big loss.
    check("memory after the suite is within the warm-up allowance",
          bool(m1 and m2) and int(m1.group(1)) - int(m2.group(1)) < 16384,
          before + after)
run("echo survives-poweroff > /mnt/boot-test-marker")
send("poweroff")
t0 = time.time()
while q.poll() is None and time.time() - t0 < 30:
    pump(0.2)
    time.sleep(0.05)
check("poweroff makes QEMU exit", q.poll() is not None)
if q.poll() is None: q.kill()
data = subprocess.run(["debugfs", "-R", "cat /mnt/boot-test-marker", disk],
                      capture_output=True, text=True).stdout
check("data written before poweroff reached the disk", "survives-poweroff" in data)
fsck = subprocess.run(["e2fsck", "-fn", disk], capture_output=True, text=True)
check("host e2fsck: disk is clean after shutdown", fsck.returncode == 0)

print("[boot] ALL PASSED" if not failures else f"[boot] {len(failures)} FAILED")
sys.exit(1 if failures else 0)
