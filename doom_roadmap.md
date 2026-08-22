# DOOM Roadmap

A deliberate detour: get DOOM running on the aarch64/RPi4 port before continuing
with Phase 3 (`SYS_FORK` / `SYS_EXECVE` / process reaping).

The point isn't DOOM. It's that DOOM is a demanding but *self-contained* workload,
so it forces the syscall layer, allocator and drivers to be genuinely correct
against a real program rather than against test payloads written to pass. Every
bug this session — the no-op `SYS_SLEEP`, the UART's per-process MMIO mapping, the
unvalidated user pointers — was found by exercising something, never by reading
code. DOOM is a much bigger something.

Target port is **doomgeneric**, which isolates the platform layer to five hooks.

## 1. What DOOM Does NOT Need

Worth stating explicitly, because these look like prerequisites and aren't:

- **Preemptive syscalls.** The interrupt-masking cost is specific to *slow I/O* —
  the UART is ~86.8us per byte, which is why a 100-byte write blocks interrupts
  for ~8.7ms. Memory operations are ~1000x faster. DOOM's hot path is writing
  pixels to a mapped framebuffer **directly at EL0**, with no `svc` at all, and
  its actual syscalls (time, key, sleep) are microsecond-scale. Revisit
  preemption when a shell prints long output while input arrives machine-fast,
  not for this.
- **`fork` / `execve` / reaping.** Not needed if the kernel boots straight into
  DOOM as the only process. Phase 3 stays deferred.
- **mlibc.** DOOM needs *a* libc, not *that* libc. See section 4.

## 2. Blocker: FP/SIMD Context Switching

`struct cpu_context` (kernel/include/kernel/proc/proc.h) saves `x19`-`x28`, `fp`,
`lr`, `sp` — and no FP/SIMD state at all. Meanwhile `boot.S` sets
`CPACR_EL1.FPEN = 0b11`, so FP/SIMD is fully enabled at both EL0 and EL1.

Two consequences:

- **Latent today.** `CFLAGS` has no `-mgeneral-regs-only`, so GCC may use FP/SIMD
  in kernel code (AAPCS64 variadic prologues do — see boot.S's own CPACR comment).
  AAPCS64 makes `d8`-`d15` callee-saved and `switch.S` doesn't preserve them.
  Hasn't bitten yet because kernel FP use is incidental and short-lived.
- **Mandatory for DOOM** the moment it shares the CPU with anything else. A
  single-process DOOM would get away with it, which is exactly the kind of thing
  that works until it silently doesn't.

Fix: extend `struct cpu_context` with `q8`-`q15` plus `FPCR`/`FPSR`, add the
`stp`/`ldp` pairs to `kernel/arch/aarch64/proc/switch.S`. Small, and a correctness
fix independent of DOOM — do it first.

## 3. Blocker: Framebuffer (the real work)

The only item with genuine unknowns. Everything else on this list is either done
or an afternoon.

- **VideoCore mailbox interface** — property-channel messages to allocate a
  framebuffer: set physical/virtual size, set depth, get the buffer's address and
  pitch. New driver, but the MMIO mapping pattern is already established twice
  (`gic.c`'s `mmio_map_device()`, and the UART's Device-memory mapping in
  `init_paging()`). Map it high, `PAGE_CACHE_DISABLE`, TTBR1-resident.
- **Map the framebuffer into DOOM's address space** so it writes pixels at EL0
  with no syscall per frame. This is what keeps preemption off the critical path.
  The VMA allocator (`kernel/mm/uvm.c`) already handles the process side.

## 4. libc: Minimal, Not mlibc (for now)

Three of the hard pieces already exist and are already exercised every boot:

- **malloc/free** — liballoc (`kernel/mm/kheap.c`). Standalone; its only OS
  dependency is `liballoc_alloc` / `liballoc_free` / `liballoc_lock`. Repoint
  those at `SYS_ANON_ALLOC` and it runs at EL0 unchanged.
- **printf** — the formatter in `kernel/console/console.c` (`%c %d %p %s %u %x
  %X`). Portable; swap `serial_putc` for `SYS_WRITE`.
- **string.h** — `kernel/klibc/` has `memcpy`, `memset`, `memmove`, `strlen`,
  `strcmp`. Real but thin.

Still needed, all short and well-specified: `strncpy`, `strcasecmp`,
`strncasecmp`, `strchr`, `strrchr`, `strstr`, `snprintf`, `vsnprintf`, `atoi`,
`strtol`, `abs`, `exit`.

**Skip stdio entirely.** The awkward part would be `fopen`/`fread`/`fseek`/`ftell`
for the WAD — but the WAD ships in the initrd and is *already in RAM*.
`tar_open()` hands back a pointer and a size; map those pages into DOOM and patch
the WAD loader to take a memory pointer instead of a file handle. Standard
practice for embedded DOOM ports, and it deletes the whole layer.

Port mlibc **after** DOOM, validated against a real application instead of a
hypothetical one. `sys_tcb_set` already exists for its TLS, so the intent stands —
just not on the critical path. Nothing here is wasted: a real libc port reuses the
same syscall layer underneath.

## 5. doomgeneric Hook Status

| Hook | Status | Needs |
|---|---|---|
| `DG_SleepMs` | done | `SYS_SLEEP` |
| `DG_GetKey` | done | `SYS_READ` |
| `DG_GetTicksMs` | trivial | syscall over the existing `timer_ticks()` |
| `DG_DrawFrame` | blocked | section 3 |
| `DG_Init` | blocked | section 3 |

## 6. Suggested Order

1. **FP/SIMD context switch** (section 2) — small, correctness fix regardless.
2. **`DG_GetTicksMs` syscall** — `timer_ticks()` / `timer_ticks_per_second()`
   already exist; this is plumbing.
3. **Mailbox + framebuffer driver** (section 3) — the actual project.
4. **Map framebuffer + WAD into the process** — VMA allocator handles it.
5. **Minimal libc** (section 4) — grind, but no unknowns.
6. **doomgeneric platform layer** — the five hooks.

Memory budget is a non-issue: classic DOOM wants ~8MB of zone, `USER_HEAP_SIZE`
is 256MB, and a 320x200x8bpp framebuffer is 64KB.

## 7. Standing Constraint

Everything interrupt-, MMU- or timing-related gets confirmed on the real Pi4, not
QEMU. QEMU's virtual clock runs orders of magnitude fast and it doesn't model
cache behaviour at all — this session it made a `SYS_SLEEP` that never slept look
merely "accelerated", and it would pass a wrong MAIR index that real silicon
rejects. Compare counts against design rates rather than eyeballing whether
output looks fast.
