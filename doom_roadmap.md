# DOOM Roadmap — DONE

Update: DOOM now builds with the real `aarch64-extron` toolchain and mlibc.
The original minimal-libc work described below remains as historical context.
The kernel now exposes one writable ramfs namespace, lazily seeded by initrd
files with copy-on-write data. Doom's upstream stdio WAD backend works through
ordinary `fopen`/`fread`/`fseek`, while configs and saves can be created in RAM.

DOOM runs on real hardware, concurrently with a second process, on one core.
`captures/doom_hardware.png` is the display; the serial log alongside it shows a
Fibonacci ticker waking on a timer once a second with `t == n` for 44 consecutive
seconds — no measurable drift while DOOM saturates the CPU.

That pairing is the actual milestone. DOOM alone shows the kernel can load and
run a demanding program, which a good program loader could also do. Two
processes with opposite CPU profiles making independent progress needs
preemption, sleep/wake, per-process address spaces, FP and SP_EL0 context
switching, the VMA allocator and an accurate timer — all at once, and none of
them aware of each other.

The rest of this file is the plan as it was written and what each step actually
cost. Phase 3 (`SYS_FORK` / `SYS_EXECVE` / process reaping) is next.

---

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

## 3. Framebuffer — DONE (`da8cdf1`, `b37ec6b`)

Working on real hardware: eight colour bars, correct geometry, no shear, correct
byte order (`captures/fb_hardware.png`). Built in two commits deliberately —
mailbox transport first, framebuffer on top — so a failure would name its own
layer.

Hardware results: VC memory `0x3b400000` + 76MiB (exactly the memory-map hole,
independently confirmed), framebuffer allocated at `0x3eace000` inside it,
640x480 32bpp RGB, `pitch == width*4` on this display though the code never
assumes that.

Three things it needed, all identified before writing the code rather than by
debugging:

- **Explicit `kmap()`** — the framebuffer is in the VC hole, and `init_paging()`
  HHDMs only AVAILABLE regions, so `phys_to_virt_hhdm()` would return a pointer
  to nothing. Same lesson as the UART in `268c962`.
- **`PAGE_NORMAL_NC`** — a new MAIR entry (index 2, `0x44`). The GPU scans the
  framebuffer out continuously so it must not sit dirty in our caches, but
  `PAGE_CACHE_DISABLE` (Device-nGnRnE) would be the wrong fix: Device memory
  forbids unaligned access unconditionally, so a `memcpy` into it can fault —
  and `DG_DrawFrame` is exactly a memcpy.
- **`BUS_TO_PHYS`** — `FB_ALLOCATE` returns a bus address in the GPU's
  0xC0000000 window.

Original notes on what made this the hard part are kept below.

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
- **The splash persists while our kernel runs**, since nothing touches it. So the
  first successful DG_DrawFrame REPLACES the rainbow — an unmistakable
  transition, not a subtle one. (A black frame captured earlier was simply the
  Pi unbooted with its SD card still in the reader — not the kernel blanking
  anything. Worth stating because "black" and "no signal" are the same picture.)
- **The active area is 4:3** (`cropdetect` -> `crop=960:720:160:0`), not the
  16:9 the capture command asks v4l2 for — the dongle scales. The Pi likely fell
  back to a default mode because a capture dongle's EDID is minimal. Concrete
  reason to use the width/height/pitch the mailbox REPORTS BACK rather than the
  values requested: they are not the same thing here.

### Where the framebuffer lives, and why that bites

The firmware's `/memory` node on this board (2GB Pi4) declares RAM with a hole:

```
Region 1:  0x00000000 - 0x3B400000     948 MiB
   HOLE:   0x3B400000 - 0x40000000      76 MiB   <- VideoCore carve-out
Region 2:  0x40000000 - 0x80000000    1024 MiB
```

The PMM's own stats confirm the hole is untouched, exactly: 524288 managed minus
504547 free = 19741 used pages; the 76MB hole alone is 19456 pages, leaving 285
(~1.1MB) for the kernel, bitmap, initrd and the reserved first 1MB. It stays
reserved *by construction* — `init_pmm()` sets the whole bitmap to 0xFF and only
clears regions the map calls AVAILABLE, so any hole is denied by default.

Two consequences:

- **The framebuffer is NOT in the HHDM.** `init_paging()` maps only
  MULTIBOOT_MEMORY_AVAILABLE regions, and the framebuffer will be in that hole,
  so `phys_to_virt_hhdm()` on the mailbox's returned address yields a pointer to
  nothing. It needs its own explicit mapping — the same lesson the UART taught
  in `268c962`, known this time BEFORE writing the driver rather than after
  debugging writes that silently went nowhere. `vm_map_region()`
  (kernel/mm/uvm.c) covers the user side; the kernel side wants the Device-memory
  `kmap()` pattern from `gic.c`'s `mmio_map_device()`.
- **This is by design, not luck.** `init_pmm()` says so in as many words —
  "Deny by Default: Mark everything as USED (1)" — and the two high-water marks
  it keeps are deliberate and distinct: `highest_reserved_addr` places the bitmap
  past `_kernel_end`, the mb2 struct and every module, so it lands in available
  low RAM after everything the bootloader put down (with an explicit panic if it
  crosses 1GB, boot.S's identity-map limit), while `total_mem` sizes the bitmap
  to the top of RAM so holes are *covered* by it. Bits for a hole are set at init
  and never cleared, because only AVAILABLE regions get freed. Anything the
  memory map doesn't positively declare usable is therefore unreachable to the
  allocator.

  Nothing parses `/memreserve/` or `/reserved-memory`, which declare a different
  thing from a hole: RAM that IS inside a usable range but is occupied right now
  (the DTB, resident firmware, CMA, a simplefb handoff) and becomes free later —
  a lifetime statement, where a hole is a permanent one. That gap is currently
  theoretical here, because everything such an entry would protect is already
  covered another way: the DTB is fully consumed before `init_pmm()` exists (its
  regions and initrd bounds are copied into locals and `dtb_phys` is never read
  again, so those pages are safe to reuse); the initrd is reserved from the
  MODULE tag; the armstub at 0x0 falls inside the unconditional first-1MB
  reservation; secondary cores are parked at `wfe` inside our own kernel text
  (boot.S) rather than in firmware spin tables; and the mb2 shim buffer is
  reserved explicitly. Worth revisiting only if one of those stops being true —
  e.g. if anything ever needs the DTB after boot.

Two known gotchas for the driver itself, worth not rediscovering: the mailbox
returns a **bus** address (typically `0xC0000000 | phys`) that must be masked
down to an ARM physical address, and the mailbox registers are MMIO at
`0xFE00B880` — same peripheral block as the UART, so same Device-memory
treatment.

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

## 4b. What DOOM found that the tests did not

The premise of this detour was that a real program exercises the system in ways
test payloads written by the same author do not. It did, immediately:

- **`vsnprintf` had no precision support.** DOOM builds HUD font lump names with
  `"STCFN%.3d"` and died looking up a lump literally called `STCFN%.3d`. Twenty-one
  hand-written libc checks all passed, because they shared the blind spots of the
  person who wrote the library.
- **`crt0` never set `argc`/`argv`.** `main()` got whatever was in x0/x1 —
  harmless until something read them.
- **The 20Hz timer made every short sleep a 50ms sleep.** `sys_sleep()` rounds a
  sub-tick request up to one tick, so `DG_SleepMs(1)` overshot 50x. DOOM's tic is
  28.6ms, so it missed every deadline it had and the game felt heavy. Now 1kHz.
  No test had ever asked for a short sleep.
- **"RGB" pixel order meant the opposite of what software wants.** Byte 0 = red
  is `0xAABBGGRR`; DOOM (like most things) composes `0xAARRGGBB`. First run came
  out entirely blue. Requesting BGR *byte* order gives the layout software
  expects.

## 5. doomgeneric Hook Status

| Hook | Status | Needs |
|---|---|---|
| `DG_SleepMs` | done | `SYS_SLEEP` |
| `DG_GetKey` | done | `SYS_READ` |
| `DG_GetTicksMs` | done | `SYS_UPTIME_MS` (6d8a159) |
| `DG_DrawFrame` | done | mapped framebuffer, no syscall in the frame loop |
| `DG_Init` | done | framebuffer mapped at exec |

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
