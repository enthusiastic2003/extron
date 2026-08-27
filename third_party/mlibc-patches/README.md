# Extron mlibc patches

The upstream mlibc checkout is intentionally not vendored in this repository.
These patches record the small generic-rtld changes contained in the checked-in
`usr/mlibc-sysroot/lib/ld.so`, so the runtime is reproducible without hiding
source changes inside an ignored checkout.

The current checkout is based on mlibc commit `9fdb2774`. Apply the patches from
the repository root with:

```sh
git -C third_party/mlibc apply ../mlibc-patches/0001-rtld-tls-dlsym-and-relro.patch
git -C third_party/mlibc apply ../mlibc-patches/0002-rtld-thread-safe-dlclose.patch
git -C third_party/mlibc apply ../mlibc-patches/0003-pathconf-sysdeps.patch
git -C third_party/mlibc apply --unidiff-zero ../mlibc-patches/0004-rtld-trace-loaded-objects.patch
```

The patch fixes generic loader behavior rather than weakening mlibc for Extron:
`dlsym()` returns the calling thread's TLS instance for `STT_TLS`, and the
loader protects complete `PT_GNU_RELRO` pages after relocation. Both behaviors
have direct QEMU integration tests in `usr/mlibc_tests/`. The second patch adds
serialized runtime-loader state, per-thread `dlerror()`, reference-counted
dependency-aware unloading, destructor ordering, DTV cleanup, and real DSO
unmapping.

The third patch replaces mlibc's aborting `fpathconf()` placeholder and its
single hard-coded `pathconf()` answer with ordinary weak sysdep hooks. Extron
then supplies those hooks in its tracked platform sysdeps, including real
pathname/descriptor validation and truthful VFS limits.

The fourth patch adds the conventional `LD_TRACE_LOADED_OBJECTS` runtime-linker
mode used by `ldd`. It reports the exact dependency graph and resolved paths
chosen by the loader, then exits before relocation, constructors, or program
entry. Extron supplies the small platform `sys_exit` hook in its tracked rtld
sysdeps.
