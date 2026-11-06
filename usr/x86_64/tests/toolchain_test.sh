#!/bin/sh
# The native C toolchain on Extron: tcc (as cc), make, ar. Everything here runs INSIDE the
# guest: compile and link (dynamic and static), a multi-file project built by make, zlib's
# own ./configure && make && make test from its source tarball, the compiler rebuilding
# itself from /usr/src, an ABI cross-check against the output gcc+glibc produced on the
# host, the AF_INET test program built by tcc, and a sweep of the system headers.
# Output format: "[toolchain] what PASS|FAIL".
D=/opt/tests/data
W=/tmp/toolchain-$$
rm -rf "$W"; mkdir -p "$W" && cd "$W" || { echo "[toolchain] workdir FAIL"; exit 1; }
t() { # t "what" command...   (passes when the command succeeds)
    what=$1; shift
    if "$@" >/dev/null 2>&1; then echo "[toolchain] $what PASS"; else echo "[toolchain] $what FAIL"; fi
}
eq() { if [ "$2" = "$3" ]; then echo "[toolchain] $1 PASS"; else echo "[toolchain] $1 FAIL (got '$3')"; fi; }

command -v cc >/dev/null && command -v make >/dev/null || { echo "[toolchain] not installed (skipped) PASS"; exit 0; }

# ---- compile, link, run ---------------------------------------------------------
cat > hello.c <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
int main(int argc, char **argv) {
    char *s = malloc(32); strcpy(s, "hello");
    errno = 0; (void)strtol("99999999999999999999", NULL, 10);
    printf("%s %d %.3f %d\n", s, argc, sqrt(2.0), errno == ERANGE);
    free(s);
    return 7;
}
C
t "cc compiles and links a program (dynamic, against libc.so)" cc hello.c -o hello -lm
./hello a b > out.txt; rc=$?
eq "...it runs and prints correctly" "hello 3 1.414 1" "$(cat out.txt)"
eq "...and returns its exit status" "7" "$rc"
t "cc -static links against libc.a" cc -static hello.c -o hello_s -lm
./hello_s > out_s.txt; eq "...the static program runs" "hello 1 1.414 1" "$(cat out_s.txt)"

# ---- a multi-file project with make and ar ----------------------------------------
cat > a.c <<'C'
int add(int x, int y) { return x + y; }
C
cat > b.c <<'C'
int mul(int x, int y) { return x * y; }
C
cat > main.c <<'C'
#include <stdio.h>
int add(int, int); int mul(int, int);
int main(void) { printf("%d\n", mul(add(2, 3), 6)); return 0; }
C
cat > Makefile <<'M'
CC = cc
prog: main.o libab.a
	$(CC) -o prog main.o libab.a
libab.a: a.o b.o
	rm -f libab.a
	ar rcs libab.a a.o b.o
%.o: %.c
	$(CC) -c $< -o $@
M
t "make builds a project with a static library (ar)" make
eq "...and the result is right" "30" "$(./prog)"
touch a.c; make >/dev/null 2>&1
eq "make knows the project is up to date after a rebuild" "1" "$(make 2>&1 | grep -c 'up to date')"

# ---- the compiler against gcc's idea of the ABI ----------------------------------------
if [ -f $D/abi.c ]; then
    cc -w $D/abi.c -o abi -lm -lpthread 2>/dev/null
    ./abi > abi.out 2>&1
    eq "ABI cross-check output is identical to gcc+glibc's (structs, varargs, callbacks, threads)" \
       "$(cat $D/abi.expected)" "$(cat abi.out)"
fi

# ---- sockets, compiled by tcc ----------------------------------------------------------
if [ -f $D/net_test.c ]; then
    cc -w $D/net_test.c -o net_test 2>/dev/null
    eq "the 40-check AF_INET test, built by tcc, passes" "1" "$(./net_test 2>&1 | grep -c 'ALL PASSED')"
fi

# ---- real software: zlib from its source tarball ------------------------------------------
if [ -f $D/zlib-1.3.1.tar.gz ]; then
    tar xzf $D/zlib-1.3.1.tar.gz && cd zlib-1.3.1
    t "zlib: ./configure finds the compiler" ./configure
    t "zlib: make builds the library, the shared library and the test programs" make
    eq "zlib: make test passes" "1" "$(make test 2>&1 | grep -c 'zlib 64-bit test OK')"
    cd "$W"
fi

# ---- the compiler rebuilds itself --------------------------------------------------------------
if [ -f /usr/src/tcc-extron-src.tar.gz ]; then
    mkdir tccbuild && cd tccbuild && tar xzf /usr/src/tcc-extron-src.tar.gz && cd tinycc-*
    t "tcc: the installed tcc builds tcc from source" make tcc libtcc1.a
    cd "$W"
    ./tccbuild/tinycc-*/tcc -B ./tccbuild/tinycc-*/ hello.c -o hello2 -lm 2>/dev/null
    ./hello2 a b > out2.txt
    eq "tcc: the rebuilt compiler produces a working program" "hello 3 1.414 1" "$(cat out2.txt)"
fi

# ---- the system headers each compile on their own -------------------------------------------------
# Known not to: complex.h (tcc has no _Complex), lastlog.h (mlibc ships no utmp.h), and the
# kernel's linux/*.h, which are meant to be included after libc headers; ffitarget.h is libffi's
# private header (ffi.h includes it).
bad=0; n=0; failed=
cd /usr/include
for h in *.h sys/*.h netinet/*.h arpa/*.h net/*.h; do
    [ -f "$h" ] || continue
    case "$h" in complex.h|lastlog.h|ffitarget.h) continue;; esac
    n=$((n + 1))
    printf '#include <%s>\nint main(void){return 0;}\n' "$h" > "$W/hs.c"
    cc -c "$W/hs.c" -o "$W/hs.o" 2>/dev/null || { bad=$((bad + 1)); failed="$failed $h"; }
done
eq "all $n system headers compile on their own with cc" "0" "$bad$failed"
cd /; rm -rf "$W"
