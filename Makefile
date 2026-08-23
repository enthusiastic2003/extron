BUILD   = build

CC      = aarch64-linux-gnu-gcc
LD      = aarch64-linux-gnu-ld
OBJCOPY = aarch64-linux-gnu-objcopy

# -mno-outline-atomics: cortex-a72 predates LSE, and outline atomics call into
#   a libatomic helper we don't have in this freestanding build.
# -mgeneral-regs-only: the kernel must never touch FP/SIMD. Exception entry
#   (SAVE_CONTEXT, exception_vectors.S) deliberately does NOT save v0-v31, so a
#   user process's FP state stays live in the hardware registers across syscalls
#   and IRQs and comes back for free on eret. That is only sound if kernel code
#   provably never clobbers those registers, which is what this flag guarantees
#   — GCC will otherwise use FP/SIMD for things unrelated to floating point
#   (AAPCS64 variadic prologues, __builtin_popcount). It also makes
#   context_switch the one correct place to save/restore FP state: at that
#   point the registers still hold the outgoing process's own values.
# -fno-store-merging: with no MMU/VBAR_EL1 yet (Milestone 1/2), all memory is
#   strict Device semantics and GCC's store-merging can synthesize unaligned
#   wide stores that fault silently forever with no exception vector to catch
#   them. Revisit once paging + exceptions land.
# -MMD -MP: emit a .d file per object listing the headers it included, so
#   editing a header actually rebuilds everything that depends on it.
#   Without this the build silently produces MISMATCHED objects: changing
#   struct cpu_context (kernel/include/kernel/proc/proc.h) once left a
#   stale sched.o allocating a 104-byte scratch context while a freshly
#   built switch.S wrote 640 bytes into it, and a stale struct proc
#   layout that read p->entry from the wrong offset. It presented as a
#   Data Abort inside context_switch — i.e. as a bug in the code that was
#   correct, which is the worst possible place for it to surface.
CFLAGS  = -ffreestanding -O2 -Wall -Wextra -nostdlib -fno-stack-protector \
          -mcpu=cortex-a72 -mno-outline-atomics -mgeneral-regs-only -fno-store-merging \
          -MMD -MP \
          -Ikernel/include -Ikernel/arch/aarch64/include -g

C_SRC   := $(shell find kernel -name "*.c")
S_SRC   := $(shell find kernel -name "*.S")
C_OBJ   := $(patsubst kernel/%.c,$(BUILD)/kernel/%.o,$(C_SRC))
S_OBJ   := $(patsubst kernel/%.S,$(BUILD)/kernel/%.o,$(S_SRC))
OBJ     := $(C_OBJ) $(S_OBJ)
DEPS    := $(C_OBJ:.o=.d) $(S_OBJ:.o=.d) $(USER_LIB_OBJ:.o=.d)

KERNEL_ELF := $(BUILD)/kernel8.elf
KERNEL_IMG := $(BUILD)/kernel8.img

# --- Userland test payloads -> initrd.tar ---
# No aarch64-extron cross toolchain exists yet, so these are plain
# freestanding static ELFs via the same aarch64-linux-gnu-gcc used for
# the kernel itself — see usr/user_test.S for why. ELF_USER_EXPECTED_BASE
# (kernel/include/kernel/elf.h) requires the first PT_LOAD segment to
# start at exactly 0x400000; -Ttext-segment (not -Ttext, which only pins
# the .text *section*, not the segment that contains the ELF/program
# headers too) is what actually guarantees that.
USER_CC      = aarch64-linux-gnu-gcc
USER_LDFLAGS = -ffreestanding -nostdlib -static -no-pie \
               -Wl,-Ttext-segment=0x400000 -Wl,--build-id=none

USER_ASM_SRC := $(wildcard usr/*.S)
USER_C_SRC   := $(wildcard usr/*.c)
USER_DATA    := $(wildcard usr/*.txt) $(wildcard usr/*.wad)

# The userspace C library. Payloads written in C (usr/*.c) link crt0 plus
# these; payloads written in assembly (usr/*.S) are freestanding and link
# none of it, which is why they keep their own _start.
#
# liballoc.c is the KERNEL's allocator source compiled a second time —
# not a copy. -Iusr/include shadows kernel/include/liballoc_config.h with
# the userspace one, which renames kmalloc/kfree to malloc/free and
# points the page hooks at SYS_ANON_ALLOC (usr/lib/malloc.c).
USER_LIB_SRC := $(wildcard usr/lib/*.c) kernel/mm/liballoc.c
USER_LIB_ASM := usr/lib/crt0.S
USER_LIB_OBJ := $(patsubst %.c,$(BUILD)/usrlib/%.o,$(USER_LIB_SRC)) \
                $(patsubst %.S,$(BUILD)/usrlib/%.o,$(USER_LIB_ASM))

USER_CFLAGS  = -ffreestanding -O2 -Wall -Wextra -nostdlib -fno-stack-protector \
               -mcpu=cortex-a72 -mno-outline-atomics -fno-builtin \
               -MMD -MP -Iusr/include -g

# --- mlibc-based userland tests (usr/mlibc_tests/) ---
# Built with the real aarch64-extron cross toolchain against this repo's
# own usr/mlibc-sysroot/ rather than aarch64-linux-gnu-gcc + usr/lib:
# these are real mlibc programs (crt1/TLS/constructors via __dlapi_enter,
# malloc, fork/execve/wait, printf) rather than the raw-syscall/hand-
# rolled-libc payloads above. The toolchain binary itself is still a
# machine-local build (see usr/mlibc_tests/mlibc_syscall_test.c's header
# comment) — MLIBC_GCC can be overridden if it doesn't live at the
# default path. The sysroot (headers + libc.a + crt0.o/crt1.o) is
# checked into this repo and confirmed sufficient on its own.
MLIBC_GCC     ?= $(HOME)/extron-toolkit/toolchain/bin/aarch64-extron-gcc
MLIBC_SYSROOT := usr/mlibc-sysroot
MLIBC_LIBC    := $(MLIBC_SYSROOT)/lib/libc.a
MLIBC_C_SRC   := $(wildcard usr/mlibc_tests/*.c)
MLIBC_ELF     := $(patsubst usr/mlibc_tests/%.c,$(BUILD)/initrd/%.elf,$(MLIBC_C_SRC))

INITRD_ELF   := $(patsubst usr/%.S,$(BUILD)/initrd/%.elf,$(USER_ASM_SRC)) \
                $(patsubst usr/%.c,$(BUILD)/initrd/%.elf,$(USER_C_SRC)) \
                $(MLIBC_ELF) \
                $(BUILD)/initrd/doom.elf

# --- DOOM ---
# doomgeneric's own sources, compiled against mlibc with the same real
# aarch64-extron toolchain as usr/mlibc_tests/. Extron intentionally has
# a writable ramfs lazily seeded from the initrd, so Doom uses its
# upstream w_file_stdc.c through ordinary mlibc stdio calls.
#
# doomgeneric_*.c for the other platforms are excluded — ours is
# usr/doom/doomgeneric_extron.c, per the port's own instructions
# ("create a file named doomgeneric_yourplatform.c").
DOOM_DIR  := third_party/doomgeneric/doomgeneric
DOOM_SRC  := dummy am_map doomdef doomstat dstrings d_event d_items d_iwad \
             d_loop d_main d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff \
             info i_cdmus i_endoom i_joystick i_scale i_sound i_system i_timer \
             memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu \
             m_misc m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights \
             p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight \
             p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main \
             r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff \
             s_sound tables v_video wi_stuff w_checksum w_file w_main w_wad \
             z_zone w_file_stdc i_input i_video doomgeneric
DOOM_OBJ  := $(patsubst %,$(BUILD)/doom/%.o,$(DOOM_SRC)) \
             $(BUILD)/doom/doomgeneric_extron.o
DOOM_DEPS := $(DOOM_OBJ:.o=.d)
DOOM_CFLAGS := --sysroot="$(abspath $(MLIBC_SYSROOT))" -O2 -Wall -Wextra \
               -MMD -MP \
               -I$(DOOM_DIR) -DNORMALUNIX -DLINUX \
               -Wno-unused-but-set-variable
INITRD_DATA  := $(patsubst usr/%,$(BUILD)/initrd/%,$(USER_DATA))
INITRD       := initrd.tar

all: $(KERNEL_IMG) $(INITRD)

$(BUILD)/kernel/%.o: kernel/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel/%.o: kernel/%.S
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_ELF): $(OBJ) kernel/arch/aarch64/linker.ld
	$(LD) -T kernel/arch/aarch64/linker.ld -o $@ $(OBJ)

$(KERNEL_IMG): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/usrlib/%.o: %.c
	mkdir -p $(dir $@)
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/usrlib/%.o: %.S
	mkdir -p $(dir $@)
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/initrd/%.elf: usr/%.S
	mkdir -p $(dir $@)
	$(USER_CC) $(USER_LDFLAGS) $< -o $@

$(BUILD)/initrd/%.elf: usr/%.c $(USER_LIB_OBJ)
	mkdir -p $(dir $@)
	$(USER_CC) $(USER_CFLAGS) $(USER_LDFLAGS) $< $(USER_LIB_OBJ) -o $@

$(BUILD)/initrd/%.elf: usr/mlibc_tests/%.c
	mkdir -p $(dir $@)
	$(MLIBC_GCC) --sysroot="$(abspath $(MLIBC_SYSROOT))" $< -o $@ -static -O1

$(BUILD)/initrd/%: usr/%
	mkdir -p $(dir $@)
	cp $< $@

$(BUILD)/doom/%.o: $(DOOM_DIR)/%.c
	mkdir -p $(dir $@)
	$(MLIBC_GCC) $(DOOM_CFLAGS) -c $< -o $@

$(BUILD)/doom/doomgeneric_extron.o: usr/doom/doomgeneric_extron.c
	mkdir -p $(dir $@)
	$(MLIBC_GCC) $(DOOM_CFLAGS) -c $< -o $@

$(BUILD)/initrd/doom.elf: $(DOOM_OBJ) $(MLIBC_LIBC)
	mkdir -p $(dir $@)
	$(MLIBC_GCC) --sysroot="$(abspath $(MLIBC_SYSROOT))" $(DOOM_OBJ) \
		-o $@ -static -O2

$(INITRD): $(INITRD_ELF) $(INITRD_DATA)
	# Explicit filenames, not "-C dir ." — the latter adds a "./" prefix
	# to every entry name (plus a directory entry itself), and tar.c's
	# tar_open() does an exact-name match with no path normalization, so
	# a caller asking for "hello.txt" would silently never find
	# "./hello.txt".
	tar -cf $@ -C $(BUILD)/initrd $(notdir $(INITRD_ELF) $(INITRD_DATA))

run: $(KERNEL_IMG) $(INITRD)
	qemu-system-aarch64 -M raspi4b -serial stdio -display none -kernel $(KERNEL_IMG) -dtb boot/bcm2711-rpi-4-b.dtb -initrd $(INITRD)

clean:
	rm -rf $(BUILD) $(INITRD)

.PHONY: all run clean

# Header dependencies emitted by -MMD. Leading '-' so a clean tree (no .d
# files yet) isn't an error.
-include $(DEPS)
-include $(DOOM_DEPS)
