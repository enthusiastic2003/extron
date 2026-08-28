# Native GNU Binutils on Extron

## Status

The port targets GNU Binutils 2.42 with the canonical triplet
`aarch64-extron`. Extron's BFD, GAS, and GNU ld target definitions are captured
in `patches/binutils/0001-aarch64-extron-target.patch`.

The initial native tool set is:

- `as` and `ld`
- `ar` and `ranlib`
- `nm`, `objcopy`, `objdump`, `readelf`, and `strip`
- `strings`, `size`, `addr2line`, and `c++filt`

GDB, gprofng, gold, simulator support, localization, and optional debuginfod and
zstd integration are deliberately excluded. They are not required for native C
toolchain bring-up.

## Build

The repository does not vendor the 372 MB expanded upstream tree. By default,
the build script uses:

```text
../extron-toolkit/src/binutils-2.42.tar.xz
```

Override that location if necessary:

```sh
BINUTILS_ARCHIVE=/absolute/path/binutils-2.42.tar.xz \
    ./tools/build_binutils.sh
```

For the standard workspace layout, run:

```sh
JOBS=8 ./tools/build_binutils.sh
```

This extracts and patches the source if needed, builds out-of-tree under
`build/binutils-2.42-native`, and stages stripped programs under
`build/binutils-stage/usr/bin`. It does not write an SD card.

Then package the existing stage into the ext2 image:

```sh
make package-binutils
```

The rootfs builder copies the programs into `/usr/bin`. BusyBox ash remains the
shell and `/bin/sh`; Binutils does not replace any BusyBox applet.

## Why the explicit sysroot library path matters

The cross compiler was originally configured with a static-only toolkit
sysroot. If that library directory wins the search order, libiberty's bundled
`getopt` and mlibc's coarse static archive objects both define the same symbols.
The build script puts this repository's `usr/mlibc-sysroot/lib` first, selecting
the deployed `libc.so`. This is a linkage correction, not a Binutils or mlibc
compatibility patch.

All staged programs are checked for the `/lib/ld.so` interpreter.

## QEMU validation

After booting the generated image, first confirm the tools are present:

```sh
as --version
ld --version
readelf --version
```

Then run:

```sh
/opt/tests/native_binutils_smoke.sh
```

The smoke test performs the complete native object workflow:

1. writes a small AArch64 assembly source under `/tmp`;
2. assembles it with Extron's native `as`;
3. inspects its symbols and ELF header;
4. creates and indexes a static archive;
5. links a native Extron executable;
6. disassembles, sizes, searches, converts, and strips the executable; and
7. executes it and expects `native binutils works` followed by a PASS line.

Existing mlibc, mmap, dynamic-linker, threading, signal, filesystem, terminal,
Nano, and Ash tests remain the regression baseline.

## GCC dependency boundary

This completes the native assembler/linker prerequisite for GCC, but GCC itself
is a separate milestone. The current Extron cross `g++` cannot link host
programs because target `libstdc++` is not installed. Before cross-building a
native GCC executable, Extron needs a working `libstdc++` plus native-host GMP,
MPFR, and MPC. ISL can initially be omitted by disabling Graphite.
