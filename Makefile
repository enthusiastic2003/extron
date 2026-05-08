AS      = nasm
CC      = x86_64-elf-gcc
LD      = x86_64-elf-gcc

CFLAGS  = -ffreestanding -O2 -Wall -Wextra -mcmodel=large \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -nostdlib \
          -Ikernel/include -Ikernel/arch/x86_64/include

USER_CFLAGS = -ffreestanding -O2 -nostdlib -mno-red-zone \
              -fno-stack-protector -no-pie -Wall -Wextra

BUILD   = build

# --- Source discovery ---
C_SRC   := $(shell find kernel -name "*.c")
ASM_SRC := $(shell find kernel -name "*.asm")

# --- Object mapping ---
C_OBJ   := $(patsubst kernel/%.c,$(BUILD)/kernel/%.o,$(C_SRC))
ASM_OBJ := $(patsubst kernel/%.asm,$(BUILD)/kernel/%.o,$(ASM_SRC))

OBJ     := $(C_OBJ) $(ASM_OBJ)

# --- Targets ---
all: $(BUILD)/kernel.elf iso

# --- Compile C ---
$(BUILD)/kernel/%.o: kernel/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# --- Assemble ---
$(BUILD)/kernel/%.o: kernel/%.asm
	mkdir -p $(dir $@)
	$(AS) -f elf64 $< -o $@

# --- Link ---
$(BUILD)/kernel.elf: $(OBJ)
	$(LD) -T kernel/linker.ld -o $@ $(OBJ) -nostdlib

# --- ISO ---
iso: $(BUILD)/kernel.elf $(BUILD)/initrd.tar
	mkdir -p iso/boot/grub
	cp $(BUILD)/kernel.elf iso/boot/kernel.elf
	cp $(BUILD)/initrd.tar iso/boot/initrd.tar
	cp boot/grub/grub.cfg iso/boot/grub/grub.cfg
	grub-mkrescue -o myos.iso iso

# --- User binaries ---
initrd/test: user/test.c user/user.ld
	$(CC) $(USER_CFLAGS) -T user/user.ld -o $@ user/test.c

# --- Initrd ---
$(BUILD)/initrd.tar: initrd/test $(shell find initrd -type f)
	mkdir -p $(BUILD)
	tar -cf $(BUILD)/initrd.tar -C initrd .

# --- Run (QEMU helper) ---
run: all
	qemu-system-x86_64 -cdrom myos.iso -serial file:kernel.log

# --- Debug (QEMU + GDB stub) ---
debug: all
	qemu-system-x86_64 -cdrom myos.iso -s -S -serial file:kernel.log

# --- Clean ---
clean:
	rm -rf $(BUILD) iso myos.iso initrd/test

.PHONY: all iso run debug clean