AS      = nasm
CC      = x86_64-elf-gcc
LD      = x86_64-elf-gcc

CFLAGS  = -ffreestanding -O2 -Wall -Wextra -mcmodel=large \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -nostdlib \
          -Ikernel/include -Ikernel/arch/x86_64/include -g

USER_CFLAGS = -ffreestanding -nostdlib -mno-red-zone \
              -fno-stack-protector -no-pie -Wall -Wextra -g \
              -mno-mmx -mno-sse -mno-sse2

BUILD   = build

# ------------------------------------------------------------------
# Kernel source discovery
# ------------------------------------------------------------------

C_SRC   := $(shell find kernel -name "*.c")
ASM_SRC := $(shell find kernel -name "*.asm")

C_OBJ   := $(patsubst kernel/%.c,$(BUILD)/kernel/%.o,$(C_SRC))
ASM_OBJ := $(patsubst kernel/%.asm,$(BUILD)/kernel/%.o,$(ASM_SRC))

OBJ     := $(C_OBJ) $(ASM_OBJ)

# ------------------------------------------------------------------
# User program discovery
# ------------------------------------------------------------------

USER_SRC   := $(wildcard user/*.c)
USER_PROGS := $(patsubst user/%.c,initrd/%,$(USER_SRC))

# ------------------------------------------------------------------
# Main targets
# ------------------------------------------------------------------

all: $(BUILD)/kernel.elf iso

# ------------------------------------------------------------------
# Compile kernel C
# ------------------------------------------------------------------

$(BUILD)/kernel/%.o: kernel/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ------------------------------------------------------------------
# Assemble kernel ASM
# ------------------------------------------------------------------

$(BUILD)/kernel/%.o: kernel/%.asm
	mkdir -p $(dir $@)
	$(AS) -f elf64 $< -o $@

# ------------------------------------------------------------------
# Link kernel
# ------------------------------------------------------------------

$(BUILD)/kernel.elf: $(OBJ)
	$(LD) -T kernel/linker.ld -o $@ $(OBJ) -nostdlib

# ------------------------------------------------------------------
# Build ALL user binaries automatically
# ------------------------------------------------------------------

initrd/%: user/%.c user/user.ld
	$(CC) $(USER_CFLAGS) -T user/user.ld -o $@ $<

# ------------------------------------------------------------------
# Build initrd tarball
# ------------------------------------------------------------------

$(BUILD)/initrd.tar: $(USER_PROGS)
	mkdir -p $(BUILD)
	tar -cf $(BUILD)/initrd.tar -C initrd .

# ------------------------------------------------------------------
# ISO
# ------------------------------------------------------------------

iso: $(BUILD)/kernel.elf $(BUILD)/initrd.tar
	mkdir -p iso/boot/grub

	cp $(BUILD)/kernel.elf iso/boot/kernel.elf
	cp $(BUILD)/initrd.tar iso/boot/initrd.tar
	cp boot/grub/grub.cfg iso/boot/grub/grub.cfg

	grub-mkrescue -o myos.iso iso

# ------------------------------------------------------------------
# Run
# ------------------------------------------------------------------

run: all
	qemu-system-x86_64 -m 3G \
		-cdrom myos.iso \
		-serial file:kernel.log

# ------------------------------------------------------------------
# Debug
# ------------------------------------------------------------------

debug: all
	qemu-system-x86_64 -m 3G \
		-cdrom myos.iso \
		-s -S \
		-serial file:kernel.log

# ------------------------------------------------------------------
# Clean
# ------------------------------------------------------------------

clean:
	rm -rf $(BUILD) iso myos.iso
	find initrd -maxdepth 1 -type f ! -name '*.tar' -delete

.PHONY: all iso run debug clean