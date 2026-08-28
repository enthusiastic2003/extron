# Native GCC on Extron

## Status

GCC 16.2.0 now runs natively on AArch64 Extron. The validated developer
image contains:

- GCC and G++ drivers;
- C and C++ front ends;
- mlibc development headers and startup objects;
- libgcc, shared libgcc unwind support, libstdc++, and libsupc++;
- native Binutils 2.42; and
- the native LTO wrapper and linker plugin.

The QEMU smoke test compiles, links, and executes a C program, then a C++
program using `std::vector`, exceptions, and `std::thread`, then repeats the C
build with `-flto`. All three paths pass.

Run the in-guest validation with:

```sh
sh /opt/tests/native_gcc_smoke.sh
```

Scripts must currently be passed to `sh` because Extron does not yet execute
`#!` interpreter scripts directly.

## Reproducing the toolchain

The upstream GCC tree is not committed. Keep the GCC 16.2.0 archive and the
GMP 6.3.0, MPFR 4.2.2, and MPC 1.3.1 archives in the default toolkit paths,
or set the archive environment variables documented by `tools/build_gcc.sh`.

```sh
tools/build_binutils.sh
tools/build_gcc.sh
make package-gcc
make run
```

`tools/build_gcc.sh` performs three distinct builds:

1. A Fedora-hosted `aarch64-extron` cross GCC plus target libgcc/libstdc++.
2. Extron-native static GMP, MPFR, and MPC libraries.
3. A Canadian-cross build whose host and target are both `aarch64-extron`.

The deployable payload is staged at `build/gcc-native-root`. The rootfs script
only includes it when that staging tree exists, so normal runtime images do
not silently rebuild or acquire a 260 MiB compiler payload.

## Extron-specific integration

The maintained upstream delta is
`patches/gcc/0001-aarch64-extron-target.patch`. It provides:

- the AArch64 Extron target and mlibc-compatible integer ABI definitions;
- `/lib/ld.so`, Extron startup-file, libc, pthread, and linker-emulation specs;
- correct shared/static libgcc and exception-unwind selection;
- native LTO-plugin shared-library support;
- the portable non-`.base64` LTO assembly representation required by native
  Binutils 2.42;
- a C++ module-reader fallback when `madvise()` is unavailable; and
- Extron recognition in GCC and bundled prerequisite configuration files.

No mlibc source was changed for GCC compatibility.

## Kernel work required by native GCC

Native compiler front ends are substantially larger than previous Extron
programs. Their bring-up removed several historical loader shortcuts:

- the user stack now resides near the top of the 48-bit TTBR0 user range;
- the runtime linker is placed after the loaded executable instead of at a
  fixed address that can overlap a large program;
- valid fixed-address ELF layouts are accepted while low/null-page mappings
  remain rejected; and
- `execve()` preserves `ENOENT` for a missing path, allowing `execvp()` and
  `posix_spawnp()` to continue searching `PATH`.

The initial shell now inherits `PATH`, `HOME`, and `TERM` from the kernel.
This environment is propagated normally through `execve()`.

## Current boundaries

- This is native compilation, not yet full self-hosting: rebuilding mlibc,
  Binutils, and GCC entirely from inside Extron remains future work.
- GCC languages are limited to C, C++, and LTO. Sanitizers, OpenMP, Fortran,
  Ada, Rust, and other runtime families are not built.
- The native developer image is about 398 MiB and consumes a large fraction
  of a 1 GiB QEMU/Raspberry Pi memory configuration while resident as initrd.
- Native C++ compilation is slow under QEMU, but completes.
- GCC plugins are staged as build support but have not received a dedicated
  in-guest plugin test.
- Shebang execution is absent, so build scripts require an explicit shell.

## Validation record

The final QEMU run validated:

```text
native C: square(9)=81
native C++: exceptions work, worker=40
native C: square(9)=81
[gcc-smoke] PASS
```

The GCC patch was also dry-run against a pristine GCC 16.2.0 tree populated
with pristine GMP, MPFR, and MPC sources, and both shell scripts pass
`bash -n`.
