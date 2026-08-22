BUILD   = build

CC      = aarch64-linux-gnu-gcc
LD      = aarch64-linux-gnu-ld
OBJCOPY = aarch64-linux-gnu-objcopy

# -mno-outline-atomics: cortex-a72 predates LSE, and outline atomics call into
#   a libatomic helper we don't have in this freestanding build.
# -fno-store-merging: with no MMU/VBAR_EL1 yet (Milestone 1/2), all memory is
#   strict Device semantics and GCC's store-merging can synthesize unaligned
#   wide stores that fault silently forever with no exception vector to catch
#   them. Revisit once paging + exceptions land.
CFLAGS  = -ffreestanding -O2 -Wall -Wextra -nostdlib -fno-stack-protector \
          -mcpu=cortex-a72 -mno-outline-atomics -fno-store-merging \
          -Ikernel/include -Ikernel/arch/aarch64/include -g

C_SRC   := $(shell find kernel -name "*.c")
S_SRC   := $(shell find kernel -name "*.S")
C_OBJ   := $(patsubst kernel/%.c,$(BUILD)/kernel/%.o,$(C_SRC))
S_OBJ   := $(patsubst kernel/%.S,$(BUILD)/kernel/%.o,$(S_SRC))
OBJ     := $(C_OBJ) $(S_OBJ)

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
USER_DATA    := $(wildcard usr/*.txt)
INITRD_ELF   := $(patsubst usr/%.S,$(BUILD)/initrd/%.elf,$(USER_ASM_SRC))
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

$(BUILD)/initrd/%.elf: usr/%.S
	mkdir -p $(dir $@)
	$(USER_CC) $(USER_LDFLAGS) $< -o $@

$(BUILD)/initrd/%: usr/%
	mkdir -p $(dir $@)
	cp $< $@

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
