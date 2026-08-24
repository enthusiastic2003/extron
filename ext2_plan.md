# ext2 Filesystem Plan — extron

## Goal

Read-only ext2 support, mounted from a real partition on the Raspberry Pi's
physical SD card — not ramfs, not an initrd view, an actual filesystem that
survives a reboot. This is the thing every VFS stage so far has been building
toward and deliberately not attempting: `kernel/fs/README.md`'s deferred list
has said "persistent/block-backed storage and an unmount protocol" since
Stage 4, and every file created at runtime today (`/tmp`, `chmod` changes,
anything DOOM saves) is gone the moment `SYS_REBOOT` fires.

**v1 scope is read-only.** Write support (block/inode allocation, bitmap
maintenance, group descriptor updates) is a real, separate project layered on
top of this one and is explicitly deferred — see "Non-goals" at the bottom.
Getting *reads* working against a real, hardware-backed, un-cooperative
filesystem is already the hard part, for a reason the next section explains.

## Why this is the hard one

Every filesystem stage before this was pure software: ramfs is a tree of
`kmalloc()`'d nodes, devfs hands back existing kernel memory. Nothing so far
has needed to talk to a piece of hardware that doesn't already have a driver.
This one does — the SD card is only reachable through the BCM2711's EMMC2
controller, and nothing in this kernel talks to it yet. Bringing up an
SD/EMMC controller bare-metal is widely known as one of the harder Raspberry
Pi peripherals to get right (clock dividers, voltage switching, command/
response timing, and a command protocol with its own state machine) —
expect this to be the majority of the effort in this plan, not the
filesystem parsing that follows it.

It's also the first piece of hardware-dependent work this project has done
where **QEMU's fidelity is unverified.** The GIC/timer work earlier in this
project's history already ran into QEMU's raspi4b model silently accepting
a wrong configuration that only real hardware exposed (see
`feedback_qemu_gic_timer_gap` if this file is ever read alongside project
memory) — assume the same risk applies here until proven otherwise. Whether
QEMU's raspi4b machine models the EMMC2 controller with enough fidelity to
develop against needs to be checked empirically, early, before relying on it
for anything in this plan. If it doesn't, this entire plan is real-hardware-
only from Stage 2 onward.

## Architecture: where this fits

A new layer sits **below** `vfs_fs_ops`, not instead of it: a block device
abstraction that the SD driver implements and ext2 consumes. ext2 itself
becomes a new `vfs_fs_ops` implementation — `kernel/fs/ext2.c` — exactly the
shape `kernel/fs/ramfs.c` and `kernel/fs/devfs.c` already are, mounted via
the same `vfs_mount_at()` every mount already goes through.

Deliberately **not** replacing ramfs as the root filesystem. Mount the ext2
partition at a subdirectory instead — `/mnt/sd`, created the same way `/bin`/
`/etc`/`/tmp` already are in `kernel/kernel.c`. Root staying ramfs means a
bug in brand-new, hardware-dependent, un-shaken-out code can't take out
`/dev`, `/bin`, or `/tmp` along with it. Moving persistent storage to the
root mount, if ever wanted, is a separate decision for well after this is
proven solid.

## Stage 1 — block device abstraction

A minimal interface, read-only, sector-addressed:

```c
struct block_device {
    int (*read)(struct block_device *dev, uint64_t lba, size_t count, void *buffer);
    uint32_t sector_size;   /* 512, virtually always */
    uint64_t sector_count;
};
```

No write hook in v1 — matches the read-only scope, and not having it means
nothing downstream can accidentally call it. `ext2.c` and the MBR parser
below are the only consumers.

## Stage 2 — the EMMC2 (SD card) controller driver

New `kernel/drivers/emmc.c` + `kernel/include/kernel/drivers/emmc.h`,
following the exact self-mapping convention `mailbox.c` and `power.c`
already establish: `kmap(NEW_HDDM + phys + off, phys + off, PAGE_PRESENT |
PAGE_WRITE | PAGE_NX | PAGE_CACHE_DISABLE)` for its own MMIO region, called
from a `emmc_init()` invoked out of `kernel_stage2()` alongside the other
driver bring-up.

Concretely:

- **MMIO base.** BCM2711's EMMC2 controller is commonly documented at
  physical `0xFE340000` — treat this as a starting point to verify against
  the BCM2711 ARM peripherals datasheet and real hardware, not as a given;
  getting this wrong looks like every other flavor of "the mailbox report
  numbers are exactly the hole in the memory map" bug already solved twice
  in this codebase — a register that reads back plausible-looking garbage
  instead of failing loudly.
- **Register model.** BCM2711's EMMC2 is SDHCI-like (the "Secure Digital
  Host Controller Simplified Specification" register layout most SoCs
  converge on) — command/argument registers, a response register set,
  block size/count, a buffer data port, present-state and interrupt-status
  registers, clock control. This is the same model enough SoCs share that
  real reference material exists to check against — this should not be
  designed from scratch by guessing.
- **Card init sequence** (standard SD Physical Layer protocol, needed
  regardless of controller):
  1. `CMD0` — GO_IDLE_STATE.
  2. `CMD8` — SEND_IF_COND, detects SDHC/SDXC vs. old SDSC cards.
  3. `ACMD41` (i.e. `CMD55` then `CMD41`) — SD_SEND_OP_COND, polled until
     the card reports ready; set the HCS bit to request high-capacity
     addressing.
  4. `CMD2` — ALL_SEND_CID.
  5. `CMD3` — SEND_RELATIVE_ADDR, get the card's RCA for every later command.
  6. `CMD9` — SEND_CSD, for capacity (useful for bounds-checking, not
     strictly required to get reads working).
  7. `CMD7` — SELECT_CARD.
  8. `CMD17`/`CMD18` — READ_SINGLE_BLOCK / READ_MULTIPLE_BLOCK, the actual
     read path `block_device.read()` calls.
- **Bring this up in complete isolation first.** Before anything in Stage 3
  or 4 exists, add one throwaway syscall or boot-time call that reads
  sector 0 and dumps its first/last 16 bytes over the console. A correct
  read of sector 0 ends with `0x55 0xAA` at bytes 510–511 (every MBR's
  signature) — that one fact is a real, checkable pass/fail signal for the
  driver alone, with no filesystem code in the way to blame instead. Don't
  start Stage 3 until this passes on real hardware.
- **The GPU firmware already used this hardware** to load `kernel8.img` and
  `initrd.tar` before our kernel ever ran. Don't assume a clean-slate
  controller state — (re-)run the full init sequence rather than
  discovering a firmware-left register value that happens to make early
  testing look like it's working.

## Stage 3 — MBR partition table

MBR, not GPT, for v1 — matches how this SD card is already laid out (a FAT32
boot partition the GPU firmware reads directly), simpler to parse, and
sufficient for one more partition.

- Read LBA 0 through the block device.
- The four 16-byte partition entries live at byte offset `0x1BE`; the whole
  sector's signature is confirmed by bytes 510–511 (`0x55 0xAA`) — the same
  check Stage 2 already used standalone.
- Find the ext2 partition either by partition type byte `0x83` (the
  conventional "Linux native" type) or simply by being the second partition
  — either is fine for a single-purpose kernel that controls its own disk
  layout.
- **Operational prerequisite, done outside the kernel:** the SD card needs a
  second partition actually created and formatted before any of this code
  can be exercised — `fdisk`/`parted` to add the partition, `mke2fs -t ext2`
  to format it, then populate it with known test files from the dev
  machine. This has to happen by hand (or via a small host-side script)
  before Stage 5's testing, and doesn't touch the existing FAT32 boot
  partition or its files (`kernel8.img`, `initrd.tar`, `config.txt`, the
  `.dtb`).

## Stage 4 — ext2 on-disk format (read path only)

New `kernel/fs/ext2.c` + `kernel/include/kernel/fs/ext2.h`, structured as one
more `vfs_fs_ops` table exactly like `ramfs_fs_ops`/`devfs_fs_ops`:

- **Superblock** at byte offset 1024 within the partition, fixed C struct
  layout, `s_magic == 0xEF53`. Read once at mount time: `s_blocks_count`,
  `s_inodes_count`, `s_blocks_per_group`, `s_inodes_per_group`,
  `s_log_block_size` (block size is `1024 << s_log_block_size`), `s_inode_size`.
- **Block group descriptor table** immediately follows the superblock's
  block. One descriptor per group: `bg_inode_table` (starting block of that
  group's inode table) is the only field the read path actually needs.
- **Inode lookup:** given an inode number `n` (1-based), group =
  `(n-1) / s_inodes_per_group`, index within group = `(n-1) % s_inodes_per_group`,
  byte offset = `group's bg_inode_table block * block_size + index * s_inode_size`.
  Read the raw `struct ext2_inode` (mode, size, the 12 direct block
  pointers, one single-indirect, one double-indirect, one triple-indirect
  pointer) from there.
- **Byte offset → block number:** the classic ext2 scheme — the first 12
  blocks of a file are direct pointers in the inode itself; beyond that, a
  single-indirect block holds up to `block_size/4` more pointers; beyond
  that, a double-indirect block of pointers to single-indirect blocks; a
  triple-indirect level exists for very large files but is unlikely to
  matter for anything this kernel will actually store — implement it for
  correctness but don't expect it to be exercised.
- **Directory entries:** a directory's data blocks hold a linear list of
  `struct ext2_dir_entry_2` (inode, `rec_len`, `name_len`, `file_type`,
  `name`) — walk them for `lookup_child`/`readdir`. No htree/`dir_index`
  support — not needed for directories this kernel will actually create by
  hand on the dev machine, and a real scope-cutter versus implementing
  ext2's hashed-tree directory format.
- **Symlinks:** ext2 stores a target under 60 bytes directly in the inode's
  block-pointer array instead of allocating a data block — handle both that
  fast path and the "target too long, stored in a real data block" case, or
  `readlink()` will work on some links and silently corrupt on others.
- **`vfs_node_ops`:** `.read` (offset/length → block(s) → block device
  reads → copy out), `.getattr` (translate the raw inode's mode/uid/gid/
  size/timestamps into `struct vfs_attr`), `.readdir`, `.readlink`.
  `.write`/`.truncate`/`.setattr` stay unset (read-only). `.mmap` is a
  reasonable v2 addition once anonymous/device mmap (already real, see
  `kernel/fs/README.md`) has an on-disk-file case to extend to — not v1.
- **`vfs_fs_ops`:** `.root`, `.lookup_child` do real work; `.create`/
  `.remove`/`.rename`/`.link`/`.symlink` all return `-EROFS` unconditionally
  — the same reason devfs's stubs exist: `vfs_open()`/`vfs_mkdir()`/etc call
  through these pointers with no null-check, so a read-only filesystem still
  has to hand back real (refusing) function pointers, not leave them unset.

## Stage 5 — mount, and testing

- `mkdir("/mnt/sd", ...)` at boot, the same place and pattern `/bin`/`/etc`/
  `/tmp` are created in `kernel/kernel.c`, then `vfs_mount_at()` the new
  `ext2_fs_ops` there once `emmc_init()` and the Stage 3 partition lookup
  have found the right partition's starting LBA.
- New `usr/mlibc_tests/mlibc_ext2_test.c`, following this suite's established
  shape (`check()` helper, `=== N failure(s) ===` summary): open known files
  under `/mnt/sd` seeded by `mke2fs`/a manual copy on the dev machine before
  flashing, and check their contents, sizes, and permissions match exactly
  what was written there — plus `readdir()` over a known directory,
  `stat()`/`lstat()` on a known symlink, and a file large enough to force at
  least the single-indirect block path (comfortably over 12 blocks; nowhere
  near needing to force double/triple-indirect on purpose).
- **Must be verified on real hardware, not QEMU alone** — consistent with
  every other hardware-adjacent rule already established in this project.
  If QEMU's raspi4b turns out to model EMMC2 well enough to be useful during
  development, treat that as a nice-to-have accelerant, never as the thing
  that gets this signed off.

## Risks / open questions

- The EMMC2 MMIO base address and the exact clock-setup sequence are the
  single biggest likely time sink — get this from a real BCM2711 peripheral
  reference rather than triangulating purely from other bare-metal Pi
  projects' source, which vary in quality and in which controller
  (EMMC vs. EMMC2 vs. legacy SDHOST) they actually target.
- QEMU raspi4b's SD/EMMC fidelity for this project is currently completely
  unknown and needs an early, explicit check (Stage 2's isolated sector-0
  read) before any other planning here is trusted.
- No hot-plug/card-removal handling — fine for a machine that's only ever
  power-cycled with the same card inserted, called out so it's a deliberate
  omission rather than a forgotten one.
- Endianness/alignment of the raw superblock/inode/dir-entry structs reading
  directly into C structs needs the same care `struct framebuffer`/mailbox
  property-tag parsing already took — pack correctly, read field-by-field
  if there's any doubt, rather than trusting a `struct` overlay blindly.

## Non-goals for v1 (explicitly deferred)

- **Write support** — block/inode allocation, free-bitmap maintenance,
  group descriptor updates, `s_state`/mount-count bookkeeping ext2 itself
  expects a real driver to touch. A completely separate, larger piece of
  work layered on top of a solid read path, not attempted here.
- **ext3/4 features** — journaling, extents, 64bit, `metadata_csum`,
  `dir_index`/htree directories. Real ext2 (revision 0/1, no journal) only.
- **GPT** partition tables — MBR only, matching the existing card layout.
- **Any locking/concurrency** design for a mutable filesystem — moot while
  read-only.
