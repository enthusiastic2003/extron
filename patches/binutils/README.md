# Extron Binutils patches

These patches are applied to an unmodified GNU Binutils 2.42 source tree by
`tools/build_binutils.sh`. The upstream source and build directory are kept out
of Git; only Extron's small, reviewable target delta is tracked.

Apply in order:

1. `0001-aarch64-extron-target.patch` teaches the canonical triplet parser, BFD,
   GAS, and GNU ld to recognize `aarch64-extron`.

The build script accepts either the archive already kept in
`../extron-toolkit/src/binutils-2.42.tar.xz` or an override through
`BINUTILS_ARCHIVE=/absolute/path/binutils-2.42.tar.xz`.
