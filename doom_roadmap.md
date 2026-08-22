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

## 2. FP/SIMD Context Switching — DONE (b4245ac)

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

Done, and larger than the sketch above: it saves **all of `v0`-`v31`** plus
`FPCR`/`FPSR`, not just `d8`-`d15`. The callee-saved subset is a *calling
convention*, and preemption doesn't land on call boundaries — every register is
potentially live. Sound only because the kernel is now built
`-mgeneral-regs-only`, so exception entry can skip FP entirely and those
registers still hold the outgoing process's values by the time `context_switch`
runs. Proven by a PAIR of test procs (`usr/fp_test_a.S`/`_b.S`); a single
FP-using proc passes even with no save at all.

## 3. Blocker: Framebuffer (the real work)

The only item with genuine unknowns. Everything else on this list is either done
or an afternoon.

**Verification rig (working, `83b4821`).** The Pi's HDMI goes to a MACROSILICON
MS2130 USB dongle; `tools/capture.sh` grabs stills or video from it. Serial can
only report what the kernel *thinks* it did — a wrong pitch, stride or pixel
order produces a picture that is visibly and diagnosably wrong while the mailbox
call still reports success.

Baseline captured before any framebuffer code exists, and it validates the
instrument rather than assuming it:

- A black frame is ~4KB; the firmware rainbow splash is ~151KB, and
  `ffmpeg -vf blackdetect` separates them cleanly. So "black" is a measurable
  result, not an ambiguous one — which matters, because a dead capture link and
  correct-but-hasn't-drawn-yet look identical otherwise.
- **The firmware has already allocated and is driving a framebuffer** (that's
  what the splash is). This work is about taking over an existing display, not
  bringing one up cold.
- **The splash persists indefinitely** while our kernel runs, since nothing
  touches it. So the first successful DG_DrawFrame REPLACES the rainbow — an
  unmistakable transition, not a subtle one.
- **The active area is 4:3** (`cropdetect` -> `crop=960:720:160:0`), not the
  16:9 the capture command asks v4l2 for — the dongle scales. The Pi likely fell
  back to a default mode because a capture dongle's EDID is minimal. Concrete
  reason to use the width/height/pitch the mailbox REPORTS BACK rather than the
  values requested: they are not the same thing here.

- **VideoCore mailbox interface** — property-channel messages to allocate a
  framebuffer: set physical/virtual size, set depth, get the buffer's address and
  pitch. New driver, but the MMIO mapping pattern is already established twice
  (`gic.c`'s `mmio_map_device()`, and the UART's Device-memory mapping in
  `init_paging()`). Map it high, `PAGE_CACHE_DISABLE`, TTBR1-resident.
- **Map the framebuffer into DOOM's address space** so it writes pixels at EL0
  with no syscall per frame. This is what keeps preemption off the critical path.
  The VMA allocator (`kernel/mm/uvm.c`) already handles the process side.

## 4. libc: Minimal, Not mlibc — DONE (3ea4891, fb7bb15)

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

Built in `usr/lib/` + `usr/include/`, with `usr/libc_test.c` as the first C
program to run on this kernel (27 checks). The allocator is **shared, not
copied**: `kheap.c` split into `kernel/mm/liballoc.c` (algorithm) and its hooks,
with `<liballoc_config.h>` supplied separately by each side — kernel gets
`kmalloc` over `vmm_alloc_pages`, userspace gets `malloc` over `SYS_ANON_ALLOC`.

The stdio skip landed as `SYS_MAP_INITRD` + `vm_map_region()`: a read-only,
NX view of an initrd file mapped straight into the process, no copy. The
framebuffer will reuse `vm_map_region()` directly.

Two bugs fell out: the user stack was ONE 4KB page (fine for assembly, hopeless
for C, no guard page — now 128KB), and `vsnprintf` padded left-justified
numerics against the running total instead of the conversion's own length.

Port mlibc **after** DOOM, validated against a real application instead of a
hypothetical one. `sys_tcb_set` already exists for its TLS, so the intent stands —
just not on the critical path. Nothing here is wasted: a real libc port reuses the
same syscall layer underneath.

## 5. doomgeneric Hook Status

| Hook | Status | Needs |
|---|---|---|
| `DG_SleepMs` | done | `SYS_SLEEP` |
| `DG_GetKey` | done | `SYS_READ` |
| `DG_GetTicksMs` | done | `SYS_UPTIME_MS` (6d8a159) |
| `DG_DrawFrame` | blocked | section 3 — the only remaining work |
| `DG_Init` | blocked | section 3 |

`DG_GetTicksMs` could NOT be built on `timer_ticks()`: the timer runs at 20Hz,
so it resolves only 50ms, and DOOM's tic rate is ~28.6ms. `SYS_UPTIME_MS` reads
`CNTPCT_EL0` (~54MHz) instead. Hardware cross-check against `SYS_SLEEP`: slept
500ms, measured 501ms.

## 6. Order — everything but the framebuffer is done

1. ~~FP/SIMD context switch~~ — done, `b4245ac`
2. ~~`DG_GetTicksMs` syscall~~ — done, `6d8a159`
3. **Mailbox + framebuffer driver** (section 3) — REMAINING, and deliberately
   taken slowly.
4. ~~Map the WAD into the process~~ — done, `fb7bb15`. The framebuffer half
   reuses the same `vm_map_region()`.
5. ~~Minimal libc~~ — done, `3ea4891`
6. **doomgeneric platform layer** — three of five hooks already work; the other
   two are section 3.

Memory budget is a non-issue: classic DOOM wants ~8MB of zone, `USER_HEAP_SIZE`
is 256MB, and a 320x200x8bpp framebuffer is 64KB.

## 7. Standing Constraint

Everything interrupt-, MMU- or timing-related gets confirmed on the real Pi4, not
QEMU. QEMU's virtual clock runs orders of magnitude fast and it doesn't model
cache behaviour at all — this session it made a `SYS_SLEEP` that never slept look
merely "accelerated", and it would pass a wrong MAIR index that real silicon
rejects. Compare counts against design rates rather than eyeballing whether
output looks fast.
