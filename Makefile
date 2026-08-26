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
DEPS    := $(C_OBJ:.o=.d) $(S_OBJ:.o=.d)

KERNEL_ELF := $(BUILD)/kernel8.elf
KERNEL_IMG := $(BUILD)/kernel8.img

# --- Userland test payloads -> initrd.tar ---
# There is no hand-rolled userspace libc in this tree — see git history
# for usr/lib/'s old crt0.S/malloc.c/stdio.c/etc, and for usr/fib_ticker.c
# /usr/key_monitor.c (the two lone usr/*.c demos that used to link it),
# if either is ever needed again. usr/*.c files (usr/reboot.c today) build
# against real mlibc via MLIBC_GCC below, same as usr/mlibc_tests/*.c,
# usr/doom/, and usr/busybox/.
USER_C_SRC   := $(wildcard usr/*.c)
USER_DATA    := $(wildcard usr/*.txt) $(wildcard usr/*.wad) usr/nano $(shell find usr/usr/local -type f 2>/dev/null)

# --- mlibc-based userland tests (usr/mlibc_tests/) ---
# Built with the real aarch64-extron cross toolchain against this repo's
# own usr/mlibc-sysroot/: real mlibc programs (crt1/TLS/constructors via
# __dlapi_enter, malloc, fork/execve/wait, printf) — every usr/ payload
# uses this same toolchain and sysroot. The toolchain binary itself is
# still a machine-local build (see usr/mlibc_tests/mlibc_syscall_test.c's
# header comment) — MLIBC_GCC can be overridden if it doesn't live at the
# default path. The sysroot (headers + libc.a + crt0.o/crt1.o) is checked
# into this repo and confirmed sufficient on its own.
MLIBC_GCC     ?= $(HOME)/extron-toolkit/toolchain/bin/aarch64-extron-gcc
MLIBC_SYSROOT := usr/mlibc-sysroot
MLIBC_LIBC    := $(MLIBC_SYSROOT)/lib/libc.a
MLIBC_LDSO    := $(MLIBC_SYSROOT)/lib/ld.so
MLIBC_LIBC_SO := $(MLIBC_SYSROOT)/lib/libc.so
MLIBC_TEST_DSO := $(BUILD)/initrd/lib/libextron_rtld_test.so
MLIBC_TEST_DEP_DSO := $(BUILD)/initrd/lib/libextron_rtld_dep.so
MLIBC_C_SRC   := $(filter-out usr/mlibc_tests/libextron_rtld_test.c \
                  usr/mlibc_tests/libextron_rtld_dep.c,\
                  $(wildcard usr/mlibc_tests/*.c))
# Built into initrd's tests/ subdirectory rather than flat at the root —
# this suite has grown to 20+ binaries, which was drowning out the
# handful of things (sh, doom.elf, reboot.elf, hello.txt) an interactive
# `ls /` actually cares about. seed_tar_file() (kernel/fs/ramfs.c) already
# splits a tar member's name on '/' and creates intermediate directories
# on the fly while seeding ramfs from the initrd, so "tests/foo.elf" in
# the archive becomes a real /tests/foo.elf without any kernel change.
MLIBC_ELF     := $(patsubst usr/mlibc_tests/%.c,$(BUILD)/initrd/tests/%.elf,$(MLIBC_C_SRC))

INITRD_ELF   := $(patsubst usr/%.c,$(BUILD)/initrd/%.elf,$(USER_C_SRC)) \
                $(MLIBC_ELF) \
                $(BUILD)/initrd/tests/mlibc_fake_interp.elf \
                $(BUILD)/initrd/doom.elf \
                $(BUILD)/initrd/sh

# --- BUSYBOX ---
BUSYBOX_DIR   := third_party/busybox
BUSYBOX_CROSS ?= $(HOME)/extron-toolkit/toolchain/bin/aarch64-extron-

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
INITRD       := initrd.ext2

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

$(BUILD)/initrd/%.elf: usr/%.c $(MLIBC_LIBC)
	mkdir -p $(dir $@)
	$(MLIBC_GCC) --sysroot="$(abspath $(MLIBC_SYSROOT))" $< -o $@ -static -O1

$(BUILD)/initrd/tests/%.elf: usr/mlibc_tests/%.c $(MLIBC_LIBC)
	mkdir -p $(dir $@)
	$(MLIBC_GCC) --sysroot="$(abspath $(MLIBC_SYSROOT))" $< -o $@ -static -O1

# First real dynamic-linker fixture. The explicit -L must precede the
# toolchain's built-in static-only sysroot, or its libc.a wins before this
# repository's libc.so is considered.
$(MLIBC_TEST_DEP_DSO): usr/mlibc_tests/libextron_rtld_dep.c
	mkdir -p $(dir $@)
	$(MLIBC_GCC) --sysroot="$(abspath $(MLIBC_SYSROOT))" -fPIC -shared \
		-nostdlib -Wl,-soname,libextron_rtld_dep.so,-z,relro,-z,now $< -o $@

$(MLIBC_TEST_DSO): usr/mlibc_tests/libextron_rtld_test.c $(MLIBC_TEST_DEP_DSO)
	mkdir -p $(dir $@)
	$(MLIBC_GCC) --sysroot="$(abspath $(MLIBC_SYSROOT))" -fPIC -shared \
		-nostdlib -L"$(abspath $(BUILD))/initrd/lib" \
		-Wl,-soname,libextron_rtld_test.so,-z,relro,-z,now $< \
		-lextron_rtld_dep -o $@

$(BUILD)/initrd/tests/mlibc_dynamic_test.elf: usr/mlibc_tests/mlibc_dynamic_test.c $(MLIBC_LDSO) $(MLIBC_LIBC_SO) $(MLIBC_TEST_DSO)
	mkdir -p $(dir $@)
	$(MLIBC_GCC) --sysroot="$(abspath $(MLIBC_SYSROOT))" \
		-L"$(abspath $(MLIBC_SYSROOT))/lib" \
		-L"$(abspath $(BUILD))/initrd/lib" -fPIE -pie \
		-Wl,--dynamic-linker=/lib/ld.so,-rpath-link,$(abspath $(BUILD))/initrd/lib \
		$< -lextron_rtld_test -o $@ -O1

# Late-loading fixture: deliberately do not link libextron_rtld_test.so here.
# Its absence from DT_NEEDED is part of the test; dlopen() must discover and
# relocate it after main() has already started.
$(BUILD)/initrd/tests/mlibc_dlopen_test.elf: usr/mlibc_tests/mlibc_dlopen_test.c $(MLIBC_LDSO) $(MLIBC_LIBC_SO) $(MLIBC_TEST_DSO)
	mkdir -p $(dir $@)
	$(MLIBC_GCC) --sysroot="$(abspath $(MLIBC_SYSROOT))" \
		-L"$(abspath $(MLIBC_SYSROOT))/lib" -fPIE -pie \
		-Wl,--dynamic-linker=/lib/ld.so $< -o $@ -O1

$(BUILD)/initrd/tests/mlibc_dlthread_test.elf: usr/mlibc_tests/mlibc_dlthread_test.c $(MLIBC_LDSO) $(MLIBC_LIBC_SO) $(MLIBC_TEST_DSO)
	mkdir -p $(dir $@)
	$(MLIBC_GCC) --sysroot="$(abspath $(MLIBC_SYSROOT))" \
		-L"$(abspath $(MLIBC_SYSROOT))/lib" -fPIE -pie \
		-Wl,--dynamic-linker=/lib/ld.so $< -o $@ -O1

# Full-RELRO fixture. Its child deliberately writes into PT_GNU_RELRO and
# must receive SIGSEGV while the parent and shell survive.
$(BUILD)/initrd/tests/mlibc_relro_test.elf: usr/mlibc_tests/mlibc_relro_test.c $(MLIBC_LDSO) $(MLIBC_LIBC_SO)
	mkdir -p $(dir $@)
	$(MLIBC_GCC) --sysroot="$(abspath $(MLIBC_SYSROOT))" \
		-L"$(abspath $(MLIBC_SYSROOT))/lib" -fPIE -pie \
		-Wl,--dynamic-linker=/lib/ld.so,-z,relro,-z,now $< -o $@ -O1

# Legacy kernel-only PT_INTERP regression fixture: this deliberately uses a
# tiny raw-syscall interpreter so failures in kernel handoff remain separable
# from failures in the real mlibc ld.so exercised by mlibc_dynamic_test.elf.
$(BUILD)/initrd/tests/mlibc_ptinterp_victim.elf: usr/mlibc_tests/mlibc_ptinterp_victim.c $(MLIBC_LIBC) tools/add_pt_interp.py
	mkdir -p $(dir $@)
	$(MLIBC_GCC) --sysroot="$(abspath $(MLIBC_SYSROOT))" $< -o $@.tmp -static -O1
	python3 tools/add_pt_interp.py $@.tmp /opt/tests/mlibc_fake_interp.elf $@
	rm -f $@.tmp

# mlibc_fake_interp.elf: hand-written freestanding AArch64 (raw syscalls,
# no mlibc startup/TLS/auxv parsing at all) standing in for a real
# dynamic linker in the PT_INTERP fixture above — see that file's own
# header comment for why it can't just be an ordinary mlibc test binary.
$(BUILD)/initrd/tests/mlibc_fake_interp.elf: usr/mlibc_tests/mlibc_fake_interp.S
	mkdir -p $(dir $@)
	$(MLIBC_GCC) -nostdlib -static -o $@ $<

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

$(BUILD)/initrd/sh: tools/configure_busybox.sh usr/busybox/extron.config $(MLIBC_LIBC)
	tools/configure_busybox.sh
	$(MAKE) -C $(BUSYBOX_DIR) -j4 CCACHE_DISABLE=1 \
		CROSS_COMPILE="$(BUSYBOX_CROSS)" \
		CONFIG_SYSROOT="$(abspath $(MLIBC_SYSROOT))"
	mkdir -p $(dir $@)
	cp $(BUSYBOX_DIR)/busybox $@

$(INITRD): $(INITRD_ELF) $(INITRD_DATA) $(MLIBC_LDSO) $(MLIBC_LIBC_SO) $(MLIBC_TEST_DSO) $(MLIBC_TEST_DEP_DSO)
	./build_ext2_root.sh

run: $(KERNEL_IMG) $(INITRD)
	qemu-system-aarch64 -M raspi4b -chardev stdio,id=serial0,signal=off -serial chardev:serial0 -display none -kernel $(KERNEL_IMG) -dtb boot/bcm2711-rpi-4-b.dtb -initrd $(INITRD)

clean:
	rm -rf $(BUILD) $(INITRD)

.PHONY: all run clean

# Header dependencies emitted by -MMD. Leading '-' so a clean tree (no .d
# files yet) isn't an error.
-include $(DEPS)
-include $(DOOM_DEPS)
