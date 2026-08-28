# External userland patches

This directory is the single source of truth for Extron's maintained changes
to external userland projects. Upstream source trees remain unvendored under
`third_party/`; the small, reviewable deltas required to reproduce each port
live here instead.

| Project | Upstream baseline | Purpose |
| --- | --- | --- |
| BusyBox | 1.38.0 | Extron/MLIBC build and shell configuration compatibility |
| mlibc | commit `9fdb2774` | Runtime linker and POSIX sysdep improvements |
| ncurses | 6.4 | Recognize the `aarch64-extron` target triplet |
| GNU Binutils | 2.42 | `aarch64-extron` target support |
| GCC | 16.2.0 | Native `aarch64-extron` C, C++, and LTO toolchain support |

Each project directory contains its patch application notes. Build helpers in
`tools/` refer only to these paths; do not keep modified upstream source files
as undocumented build inputs.
