#!/bin/sh
set -eu
PATH=/usr/bin:/bin
export PATH

work=/tmp/binutils-smoke.$$
trap 'rm -rf "$work"' 0 HUP INT TERM
mkdir "$work"

cat > "$work/hello.s" <<'ASM'
	.section .rodata
message:
	.ascii "native binutils works\n"
	.set message_length, . - message

	.text
	.global _start
_start:
	mov x0, #1
	adr x1, message
	mov x2, #message_length
	mov x8, #1
	svc #0
	mov x0, #0
	mov x8, #7
	svc #0
ASM

as -o "$work/hello.o" "$work/hello.s"
readelf -h "$work/hello.o" | grep -q 'AArch64'
nm "$work/hello.o" | grep -q '_start'
ar rcs "$work/libhello.a" "$work/hello.o"
ranlib "$work/libhello.a"
nm "$work/libhello.a" | grep -q '_start'
ld -e _start -o "$work/hello" "$work/hello.o"
readelf -h "$work/hello" | grep -q 'EXEC'
objdump -d "$work/hello" | grep -q '<_start>'
size "$work/hello" >/dev/null
strings "$work/hello" | grep -q 'native binutils works'
objcopy -O binary "$work/hello" "$work/hello.bin"
cp "$work/hello" "$work/hello.stripped"
strip "$work/hello.stripped"
"$work/hello"

echo 'native Binutils smoke test: PASS'
