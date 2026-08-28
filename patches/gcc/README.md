# GCC port patch

`0001-aarch64-extron-target.patch` is the maintained Extron delta against
upstream GCC 16.2.0. The upstream source tree and build products are ignored;
the patch and `tools/build_gcc.sh` are the reproducible source of the port.

The patch adds the `aarch64-extron` target, mlibc-compatible ABI defaults,
native shared-library/LTO support, and compatibility with the native Binutils
2.42 assembler. It also teaches the bundled GMP, MPFR, and MPC `config.sub`
files to recognize Extron.

Build the compiler with:

```sh
tools/build_gcc.sh
make package-gcc
```

The build is a Canadian cross: Fedora builds an Extron cross compiler, that
compiler builds Extron-native GMP/MPFR/MPC, and those pieces build the GCC
driver and front ends which run inside Extron.
