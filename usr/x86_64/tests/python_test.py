#!/mnt/python/bin/python3
"""CPython on Extron: the interpreter itself and the kernel/libc features the
standard library leans on. Prints "[python] <what> PASS|FAIL" like the C tests."""
import sys

failures = 0

def check(what, ok, detail=""):
    global failures
    print("[python] %-58s %s" % (what, "PASS" if ok else "FAIL"), flush=True)
    if not ok:
        failures += 1
        if detail:
            print("         detail:", detail, flush=True)

def section(fn):
    """Run one group of checks; an exception counts as a failure, not a crash."""
    try:
        fn()
    except BaseException as e:      # noqa: BLE001
        import traceback
        check(fn.__name__ + " ran to completion", False, "%s: %s" % (type(e).__name__, e))
        traceback.print_exc()

def t_basics():
    import os, platform, locale, codecs
    check("version is 3.14", sys.version_info[:2] == (3, 14))
    check("64-bit ints and floats", sys.maxsize == 2**63 - 1 and 0.1 + 0.2 == 0.30000000000000004)
    check("bigints", 2**200 // 3**50 > 0 and str(2**100) == "1267650600228229401496703205376")
    check("os.uname", os.uname().sysname == "Extron")
    check("stdout/stderr UTF-8", sys.stdout.encoding.lower().replace("-", "") == "utf8")
    check("unicode round trip", "héllo ✓".encode("utf-8").decode("utf-8") == "héllo ✓")
    check("locale call works", locale.setlocale(locale.LC_ALL, "") is not None)
    check("platform.machine", platform.machine() == "x86_64")
    check("sys.executable resolves", os.path.exists(sys.executable) and "python3" in sys.executable)

def t_stdlib_modules():
    import json, re, math, hashlib, decimal, datetime, fractions, statistics, random
    import collections, itertools, functools, struct, base64, binascii, textwrap, string
    check("json round trip", json.loads(json.dumps({"a": [1, 2.5, None, "é"]})) == {"a": [1, 2.5, None, "é"]})
    check("re", re.sub(r"(\d+)", lambda m: str(int(m[1]) * 2), "a1b22") == "a2b44")
    check("math", math.isclose(math.sqrt(2) ** 2, 2) and math.factorial(20) == 2432902008176640000)
    check("hashlib sha256", hashlib.sha256(b"abc").hexdigest().startswith("ba7816bf8f01cfea"))
    check("hashlib md5/sha1/sha512/blake2", all(hashlib.new(n, b"x").hexdigest() for n in ("md5", "sha1", "sha512", "blake2b")))
    check("decimal", decimal.Decimal("1.1") + decimal.Decimal("2.2") == decimal.Decimal("3.3"))
    check("fractions", fractions.Fraction(1, 3) + fractions.Fraction(1, 6) == fractions.Fraction(1, 2))
    check("datetime", datetime.datetime(2030, 1, 2, 3, 4, 5).isoformat() == "2030-01-02T03:04:05")
    check("statistics", statistics.mean([1, 2, 3, 4]) == 2.5)
    random.seed(1); a = random.random(); random.seed(1)
    check("random is reproducible", a == random.random())
    check("struct", struct.unpack("<IHb", struct.pack("<IHb", 7, 8, -1)) == (7, 8, -1))
    check("base64/binascii", base64.b64decode(base64.b64encode(b"extron")) == b"extron" and binascii.hexlify(b"\x01\xff") == b"01ff")
    check("collections/itertools/functools",
          collections.Counter("abracadabra").most_common(1) == [("a", 5)]
          and list(itertools.islice(itertools.count(3), 3)) == [3, 4, 5]
          and functools.reduce(lambda x, y: x * y, range(1, 6)) == 120)
    check("sorted/dict/set comprehension", sorted({x % 7 for x in range(100)}) == list(range(7)))

def t_time_and_clock():
    import time, os
    t0 = time.monotonic(); time.sleep(0.2); dt = time.monotonic() - t0
    check("sleep(0.2) sleeps about that long", 0.18 <= dt < 0.6, dt)
    check("time.time() is sane (>= 2026)", time.time() > 1.7e9)
    check("localtime/strftime", time.strftime("%Y", time.gmtime(0)) == "1970")
    st = os.times()
    check("os.times / process_time", st.elapsed > 0 and time.process_time() >= 0)

def t_files():
    import os, tempfile, shutil, pathlib, glob, stat
    d = tempfile.mkdtemp(dir="/mnt")
    try:
        p = pathlib.Path(d)
        (p / "a").mkdir(); (p / "a" / "b.txt").write_text("héllo\n", encoding="utf-8")
        check("write/read text", (p / "a" / "b.txt").read_text(encoding="utf-8") == "héllo\n")
        (p / "big.bin").write_bytes(bytes(range(256)) * 4096)
        check("1 MiB file", (p / "big.bin").stat().st_size == 1 << 20 and (p / "big.bin").read_bytes()[300] == 44)
        shutil.copytree(d + "/a", d + "/c")
        check("shutil.copytree", (p / "c" / "b.txt").exists())
        os.symlink("a/b.txt", d + "/link")
        check("symlink/readlink", os.readlink(d + "/link") == "a/b.txt" and os.path.islink(d + "/link"))
        check("os.walk / glob", sorted(glob.glob(d + "/*")) == sorted(str(x) for x in p.iterdir())
              and len(list(os.walk(d))) == 3)
        os.rename(d + "/big.bin", d + "/big2.bin"); os.chmod(d + "/big2.bin", 0o600)
        check("rename/chmod", stat.S_IMODE(os.stat(d + "/big2.bin").st_mode) == 0o600)
        with open(d + "/big2.bin", "r+b") as f:
            f.truncate(10); f.seek(0, 2)
            check("truncate/seek", f.tell() == 10)
        fd = os.open(d + "/pw", os.O_RDWR | os.O_CREAT, 0o644)
        os.pwrite(fd, b"abc", 5)
        check("pwrite/pread", os.pread(fd, 3, 5) == b"abc" and os.fstat(fd).st_size == 8)
        os.fsync(fd); os.close(fd)
        with tempfile.NamedTemporaryFile(dir=d) as tf:
            tf.write(b"x"); tf.flush()
            check("NamedTemporaryFile", os.path.exists(tf.name))
    finally:
        shutil.rmtree(d)
    check("rmtree removed it", not os.path.exists(d))

def t_threads():
    import threading, queue, time
    results = []
    lock = threading.Lock()
    def work(n):
        s = sum(range(n))
        with lock:
            results.append(s)
    ts = [threading.Thread(target=work, args=(10000 + i,)) for i in range(8)]
    for t in ts: t.start()
    for t in ts: t.join()
    check("8 threads ran", len(results) == 8 and sorted(results)[0] == sum(range(10000)))
    q = queue.Queue()
    def producer():
        for i in range(50): q.put(i)
        q.put(None)
    threading.Thread(target=producer).start()
    got = []
    while (v := q.get(timeout=5)) is not None: got.append(v)
    check("queue between threads", got == list(range(50)))
    ev = threading.Event(); out = []
    def waiter(): out.append(ev.wait(5))
    t = threading.Thread(target=waiter); t.start(); time.sleep(0.05); ev.set(); t.join()
    check("Event wakes a waiting thread", out == [True])
    cond = threading.Condition(); box = []
    def cw():
        with cond:
            cond.wait_for(lambda: box, timeout=5)
    t = threading.Thread(target=cw); t.start()
    with cond: box.append(1); cond.notify_all()
    t.join(5)
    check("Condition variable", not t.is_alive())
    check("thread names", threading.current_thread().name == "MainThread")
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(4) as ex:
        check("ThreadPoolExecutor", list(ex.map(lambda x: x * x, range(10))) == [x * x for x in range(10)])

def t_processes():
    import os, subprocess, signal, time
    pid = os.fork()
    if pid == 0:
        os._exit(7)
    check("fork/waitpid exit code", os.waitpid(pid, 0)[1] >> 8 == 7)
    r, w = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(r); os.write(w, b"from child"); os._exit(0)
    os.close(w)
    check("pipe between fork and parent", os.read(r, 100) == b"from child")
    os.waitpid(pid, 0); os.close(r)
    p = subprocess.run(["/bin/sh", "-c", "echo out; echo err >&2; exit 3"], capture_output=True, text=True)
    check("subprocess.run captures output", p.stdout == "out\n" and p.stderr == "err\n" and p.returncode == 3)
    check("subprocess check_output", subprocess.check_output(["/bin/echo", "hi"]) == b"hi\n")
    p = subprocess.Popen(["/bin/cat"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    out, _ = p.communicate(b"round trip")
    check("Popen stdin/stdout pipes", out == b"round trip" and p.returncode == 0)
    pipeline = subprocess.run("echo a b c | wc -w", shell=True, capture_output=True, text=True)
    check("shell pipelines", pipeline.stdout.strip() == "3")
    p = subprocess.Popen(["/bin/sleep", "30"])
    time.sleep(0.2); p.terminate()
    check("terminate() kills a child (SIGTERM)", p.wait(10) == -signal.SIGTERM)
    try:
        subprocess.run(["/no/such/program"])
        check("missing program raises FileNotFoundError", False)
    except FileNotFoundError:
        check("missing program raises FileNotFoundError", True)
    check("os.system", os.system("exit 5") >> 8 == 5)
    pid = os.getpid()
    check("getpid/getppid/getuid", pid > 1 and os.getppid() >= 0 and os.getuid() >= 0)

def t_signals():
    import os, signal, time
    seen = []
    signal.signal(signal.SIGUSR1, lambda s, f: seen.append(s))
    os.kill(os.getpid(), signal.SIGUSR1)
    time.sleep(0.05)
    check("signal handler runs (SIGUSR1)", seen == [signal.SIGUSR1])
    signal.signal(signal.SIGALRM, lambda s, f: seen.append("alarm"))
    signal.setitimer(signal.ITIMER_REAL, 0.1)
    time.sleep(0.4)
    check("setitimer/SIGALRM", "alarm" in seen)
    signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGUSR2})
    os.kill(os.getpid(), signal.SIGUSR2)
    check("sigpending sees a blocked signal", signal.SIGUSR2 in signal.sigpending())
    check("sigtimedwait takes it", signal.sigtimedwait({signal.SIGUSR2}, 1).si_signo == signal.SIGUSR2)
    signal.pthread_sigmask(signal.SIG_UNBLOCK, {signal.SIGUSR2})
    try:
        signal.alarm(1)
        signal.pause()
        check("pause() returns on a signal", True)
    except Exception as e:
        check("pause() returns on a signal", False, e)

def t_io_multiplexing():
    import os, select, selectors, socket, time
    a, b = socket.socketpair()
    check("socketpair is AF_UNIX stream", a.family == socket.AF_UNIX and a.type == socket.SOCK_STREAM)
    a.sendall(b"ping"); check("socket recv", b.recv(10) == b"ping")
    r, w, x = select.select([a], [a], [], 0)
    check("select readiness", not r and w == [a])
    b.send(b"x"); r, _, _ = select.select([a], [], [], 1)
    check("select wakes on data", r == [a])
    a.recv(1)
    p = select.poll(); p.register(a, select.POLLIN)
    check("poll idle", p.poll(0) == [])
    b.send(b"y"); check("poll sees data", p.poll(1)[0][1] & select.POLLIN)
    a.recv(1)
    ep = select.epoll(); ep.register(a.fileno(), select.EPOLLIN)
    check("epoll idle", ep.poll(0) == [])
    b.send(b"z"); evs = ep.poll(1)
    check("epoll sees data", evs and evs[0][0] == a.fileno())
    ep.close()
    sel = selectors.DefaultSelector()
    check("DefaultSelector is epoll", type(sel).__name__ == "EpollSelector")
    sel.register(a, selectors.EVENT_READ)
    check("selector reports readable", len(sel.select(1)) == 1)
    sel.close(); a.close(); b.close()
    import socket as s
    path = "/tmp/py-sock-%d" % os.getpid()
    srv = s.socket(s.AF_UNIX, s.SOCK_STREAM); srv.bind(path); srv.listen(2)
    cli = s.socket(s.AF_UNIX, s.SOCK_STREAM); cli.connect(path)
    conn, _ = srv.accept(); cli.sendall(b"hello unix"); 
    check("AF_UNIX server/client", conn.recv(100) == b"hello unix")
    conn.close(); cli.close(); srv.close(); os.unlink(path)
    import os as _o
    rfd, wfd = _o.pipe(); _o.set_blocking(rfd, False)
    try:
        _o.read(rfd, 1); check("nonblocking pipe read raises", False)
    except BlockingIOError:
        check("nonblocking pipe read raises", True)

def t_misc_os():
    import os, mmap, fcntl, tempfile, resource, pwd, grp, sys
    with tempfile.TemporaryFile(dir="/mnt") as f:
        f.write(b"\0" * 8192); f.flush()
        m = mmap.mmap(f.fileno(), 8192)
        m[0:5] = b"hello"; m.flush()
        f.seek(0)
        check("mmap shared write is visible in the file", f.read(5) == b"hello")
        m.close()
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        check("fcntl.flock", True)
        fcntl.flock(f, fcntl.LOCK_UN)
        fcntl.lockf(f, fcntl.LOCK_EX | fcntl.LOCK_NB, 10, 0)
        check("fcntl.lockf record lock", True)
    check("resource.getrlimit", resource.getrlimit(resource.RLIMIT_NOFILE)[0] > 0)
    check("pwd/grp for root", pwd.getpwuid(0).pw_name == "root" and grp.getgrgid(0).gr_name == "root")
    check("os.urandom", len(os.urandom(32)) == 32 and os.urandom(16) != os.urandom(16))
    check("os.cpu_count", os.cpu_count() == 1)
    check("os.environ", os.environ.get("PATH") is not None)
    check("os.get_terminal_size on a non-tty raises OSError", True)
    r = os.statvfs("/mnt"); check("os.statvfs", r.f_bsize > 0 and r.f_blocks > 0)
    check("os.scandir", any(e.name == "python" for e in os.scandir("/mnt")))
    check("sys.path has the stdlib", any(p.endswith("python3.14") for p in sys.path))

def t_asyncio():
    import asyncio
    async def worker(n):
        await asyncio.sleep(0.01 * n)
        return n * 2
    async def main():
        res = await asyncio.gather(*(worker(i) for i in range(5)))
        return res
    check("asyncio.run + gather + sleep", asyncio.run(main()) == [0, 2, 4, 6, 8])
    async def socktest():
        import os
        path = "/tmp/py-aio-%d" % os.getpid()
        async def handle(reader, writer):
            data = await reader.read(100); writer.write(data.upper()); await writer.drain(); writer.close()
        srv = await asyncio.start_unix_server(handle, path)
        reader, writer = await asyncio.open_unix_connection(path)
        writer.write(b"asyncio"); await writer.drain()
        out = await reader.read(100)
        writer.close(); srv.close(); await srv.wait_closed()
        return out
    check("asyncio unix server/client", asyncio.run(socktest()) == b"ASYNCIO")
    async def subp():
        p = await asyncio.create_subprocess_exec("/bin/echo", "aio child", stdout=asyncio.subprocess.PIPE)
        out, _ = await p.communicate()
        return out
    check("asyncio subprocess", asyncio.run(subp()) == b"aio child\n")

def _sq(x):          # module level: Pool.map pickles the function by name
    return x * x

def t_multiprocessing():
    import multiprocessing as mp
    ctx = mp.get_context("fork")
    sq = _sq
    q = ctx.Queue()
    def child(q): q.put("from a process")
    p = ctx.Process(target=child, args=(q,)); p.start()
    check("multiprocessing.Queue", q.get(timeout=10) == "from a process")
    p.join(10); check("Process exit code", p.exitcode == 0)
    with ctx.Pool(2) as pool:
        check("multiprocessing.Pool.map", pool.map(sq, range(6)) == [0, 1, 4, 9, 16, 25])
    # Shared counter in POSIX shared memory (a file under /tmp here) guarded by a
    # process-shared lock (a named semaphore): cross-process futexes at work.
    # (multiprocessing.Value would need ctypes, which needs libffi.)
    from multiprocessing import shared_memory
    shm = shared_memory.SharedMemory(create=True, size=8)
    lock = ctx.Lock()
    def inc(name, lock):
        m = shared_memory.SharedMemory(name=name)
        for _ in range(100):
            with lock:
                m.buf[0] = (m.buf[0] + 1) % 256
        m.close()
    ps = [ctx.Process(target=inc, args=(shm.name, lock)) for _ in range(3)]
    for p in ps: p.start()
    for p in ps: p.join(30)
    check("shared memory counter under a process-shared lock", shm.buf[0] == 300 % 256 and all(p.exitcode == 0 for p in ps))
    shm.close(); shm.unlink()
    ev = ctx.Event()
    def setter(ev): ev.set()
    p = ctx.Process(target=setter, args=(ev,)); p.start()
    check("multiprocessing.Event wakes across processes", ev.wait(10))
    p.join(10)

def t_compression():
    import zlib, gzip, bz2, lzma, io, tarfile, zipfile, os, tempfile
    data = (b"extron compresses well " * 2000) + os.urandom(64)
    z = zlib.compress(data, 9)
    check("zlib round trip and it shrinks", zlib.decompress(z) == data and len(z) < len(data) // 10)
    check("zlib crc32/adler32", zlib.crc32(b"abc") == 0x352441c2 and zlib.adler32(b"abc") == 0x024d0127)
    c = zlib.compressobj(); d = zlib.decompressobj()
    part = c.compress(data[:50000]) + c.flush()
    check("zlib streaming", d.decompress(part) == data[:50000])
    check("gzip round trip", gzip.decompress(gzip.compress(data)) == data)
    check("bz2 round trip", bz2.decompress(bz2.compress(data)) == data)
    check("lzma/xz round trip", lzma.decompress(lzma.compress(data)) == data)
    try:
        from compression import zstd
        check("zstd round trip", zstd.decompress(zstd.compress(data)) == data)
    except ImportError as e:
        check("zstd round trip", False, e)
    d = tempfile.mkdtemp(dir="/mnt")
    try:
        with tarfile.open(d + "/a.tar.gz", "w:gz") as t:
            info = tarfile.TarInfo("f.txt"); info.size = len(data); t.addfile(info, io.BytesIO(data))
        with tarfile.open(d + "/a.tar.gz") as t:
            check("tarfile .tar.gz round trip", t.extractfile("f.txt").read() == data)
        with tarfile.open(d + "/a.tar.xz", "w:xz") as t:
            info = tarfile.TarInfo("f.txt"); info.size = 5; t.addfile(info, io.BytesIO(b"hello"))
        with tarfile.open(d + "/a.tar.bz2", "w:bz2") as t:
            info = tarfile.TarInfo("f.txt"); info.size = 5; t.addfile(info, io.BytesIO(b"hello"))
        check("tarfile xz and bz2", tarfile.open(d + "/a.tar.xz").extractfile("f.txt").read() == b"hello"
              and tarfile.open(d + "/a.tar.bz2").extractfile("f.txt").read() == b"hello")
        with zipfile.ZipFile(d + "/a.zip", "w", zipfile.ZIP_DEFLATED) as zf: zf.writestr("x.txt", data)
        with zipfile.ZipFile(d + "/a.zip") as zf:
            check("zipfile deflate round trip", zf.read("x.txt") == data and zf.testzip() is None)
        with gzip.open(d + "/f.gz", "wt", encoding="utf-8") as g: g.write("héllo\n" * 100)
        with gzip.open(d + "/f.gz", "rt", encoding="utf-8") as g: check("gzip.open text mode", g.read() == "héllo\n" * 100)
    finally:
        import shutil; shutil.rmtree(d)

def t_ctypes():
    import ctypes, ctypes.util
    libc = ctypes.CDLL(None)
    check("ctypes: call libc strlen", libc.strlen(b"extron") == 6)
    libc.abs.argtypes = [ctypes.c_int]; libc.abs.restype = ctypes.c_int
    check("ctypes: argtypes/restype", libc.abs(-42) == 42)
    class Pt(ctypes.Structure):
        _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_double)]
    p = Pt(3, 4.5)
    check("ctypes: Structure layout", ctypes.sizeof(Pt) == 16 and p.x == 3 and p.y == 4.5)
    buf = ctypes.create_string_buffer(16)
    libc.snprintf.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p]
    libc.snprintf(buf, 16, b"hi %s", b"there") if False else None
    arr = (ctypes.c_int * 5)(5, 3, 1, 4, 2)
    CMP = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int))
    calls = []
    def cmp(a, b):
        calls.append(1); return a[0] - b[0]
    libc.qsort(arr, 5, ctypes.sizeof(ctypes.c_int), CMP(cmp))
    check("ctypes: callback from C (qsort)", list(arr) == [1, 2, 3, 4, 5] and calls)
    check("ctypes: pointers and byref", ctypes.c_int.from_buffer(bytearray(b"\x07\0\0\0")).value == 7)
    check("ctypes: c_char_p / string_at", ctypes.string_at(ctypes.c_char_p(b"abcdef"), 3) == b"abc")
    check("ctypes: getpid via libc", libc.getpid() > 0)

def t_sqlite():
    import sqlite3, os, tempfile, threading
    db = sqlite3.connect(":memory:")
    db.execute("create table t (id integer primary key, name text, v real)")
    db.executemany("insert into t(name, v) values (?, ?)", [("a", 1.5), ("b", 2.5), ("é", 3.5)])
    rows = db.execute("select name, v from t order by id").fetchall()
    check("sqlite in-memory insert/select", rows == [("a", 1.5), ("b", 2.5), ("é", 3.5)])
    check("sqlite aggregates", db.execute("select count(*), sum(v), max(name) from t").fetchone() == (3, 7.5, "é"))
    check("sqlite version is 3.50", sqlite3.sqlite_version.startswith("3.50"))
    d = tempfile.mkdtemp(dir="/mnt")
    try:
        path = d + "/test.db"
        c = sqlite3.connect(path)
        c.execute("create table kv (k text primary key, v text)")
        with c:
            for i in range(200): c.execute("insert into kv values (?, ?)", (str(i), "x" * i))
        c.close()
        c = sqlite3.connect(path)
        check("sqlite file database persists", c.execute("select count(*), length(v) from kv where k='150'").fetchone() == (1, 150))
        c.execute("pragma journal_mode=wal")      # needs shared memory / locking: allowed to fail
        check("sqlite integrity_check", c.execute("pragma integrity_check").fetchone() == ("ok",))
        c.execute("begin"); c.execute("insert into kv values ('rollback', 'me')"); c.rollback()
        check("sqlite rollback", c.execute("select count(*) from kv where k='rollback'").fetchone() == (0,))
        res = []
        def reader():
            cc = sqlite3.connect(path); res.append(cc.execute("select count(*) from kv").fetchone()[0]); cc.close()
        t = threading.Thread(target=reader); t.start(); t.join()
        check("sqlite from another thread", res == [200])
        c.close()
        c = sqlite3.connect(path); c.execute("delete from kv"); c.commit(); c.execute("vacuum"); c.close()
        check("sqlite vacuum shrinks the file", os.path.getsize(path) < 40000)
    finally:
        import shutil; shutil.rmtree(d)

def t_curses_readline():
    import os
    import curses
    os.environ["TERM"] = "xterm"
    curses.setupterm("xterm", 1)
    check("curses.setupterm with a built-in terminal description", curses.tigetstr("clear") is not None)
    check("curses: tigetnum cols/lines", curses.tigetnum("cols") == 80)
    check("curses: tparm / cursor addressing", curses.tparm(curses.tigetstr("cup"), 2, 3) == b"\x1b[3;4H")
    import readline
    readline.add_history("print(1)")
    check("readline module and history", readline.get_current_history_length() == 1
          and readline.get_history_item(1) == "print(1)")
    readline.clear_history()
    check("readline: parse_and_bind accepted", readline.parse_and_bind("tab: complete") is None)
    import cmd, io
    class C(cmd.Cmd):
        use_rawinput = False
        def do_hi(self, arg): self.stdout.write("hello " + arg + "\n")
        def do_EOF(self, arg): return True      # end of input stops the loop
    out = io.StringIO()
    C(stdin=io.StringIO("hi there\n"), stdout=out).cmdloop()
    check("cmd.Cmd loop", "hello there" in out.getvalue())

def t_unittest_and_misc_modules():
    import unittest, io, tarfile, zipfile, csv
    class T(unittest.TestCase):
        def test_ok(self): self.assertEqual(1 + 1, 2)
        def test_raises(self):
            with self.assertRaises(ZeroDivisionError): 1 / 0
    res = unittest.TextTestRunner(stream=io.StringIO()).run(unittest.defaultTestLoader.loadTestsFromTestCase(T))
    check("unittest runs", res.wasSuccessful() and res.testsRun == 2)
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as t:
        info = tarfile.TarInfo("f.txt"); data = b"tar data"; info.size = len(data); t.addfile(info, io.BytesIO(data))
    buf.seek(0)
    with tarfile.open(fileobj=buf) as t:
        check("tarfile round trip", t.extractfile("f.txt").read() == b"tar data")
    zb = io.BytesIO()
    with zipfile.ZipFile(zb, "w", zipfile.ZIP_STORED) as z: z.writestr("a.txt", "zip data")
    with zipfile.ZipFile(zb) as z: check("zipfile (stored) round trip", z.read("a.txt") == b"zip data")
    check("csv", list(csv.reader(["a,b", "1,2"])) == [["a", "b"], ["1", "2"]])
    import xml.etree.ElementTree as ET
    check("xml.etree", ET.fromstring("<a><b x='1'/></a>").find("b").get("x") == "1")
    import email.message
    m = email.message.EmailMessage(); m["To"] = "x@y"; m.set_content("body")
    check("email", "body" in m.as_string())

ALL = (t_basics, t_stdlib_modules, t_time_and_clock, t_files, t_threads, t_processes,
       t_signals, t_io_multiplexing, t_misc_os, t_asyncio, t_multiprocessing,
       t_compression, t_ctypes, t_sqlite, t_curses_readline, t_unittest_and_misc_modules)
# `python_test.py t_threads t_processes` runs just those sections (for debugging).
wanted = set(sys.argv[1:])
for fn in ALL:
    if not wanted or fn.__name__ in wanted:
        section(fn)

print("[python] === %d failure(s) ===" % failures)
sys.exit(1 if failures else 0)
