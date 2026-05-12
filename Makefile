AS      = nasm
CC      = x86_64-elf-gcc
LD      = x86_64-elf-gcc

CFLAGS  = -ffreestanding -O2 -Wall -Wextra -mcmodel=large \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -nostdlib \
          -Ikernel/include -Ikernel/arch/x86_64/include -g

USER_CC = x86_64-extron-gcc

# Userspace
USER_CFLAGS = -Wall -Wextra -g \
              -mno-mmx -mno-sse -mno-sse2 \

USER_LDFLAGS = -no-pie

BUILD   = build

C_SRC   := $(shell find kernel -name "*.c")
ASM_SRC := $(shell find kernel -name "*.asm")
C_OBJ   := $(patsubst kernel/%.c,$(BUILD)/kernel/%.o,$(C_SRC))
ASM_OBJ := $(patsubst kernel/%.asm,$(BUILD)/kernel/%.o,$(ASM_SRC))
OBJ     := $(C_OBJ) $(ASM_OBJ)

USER_SRC   := $(wildcard usr/*.c)
USER_PROGS := $(patsubst usr/%.c,initrd/%,$(USER_SRC))

all: $(BUILD)/kernel.elf iso

$(BUILD)/kernel/%.o: kernel/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel/%.o: kernel/%.asm
	mkdir -p $(dir $@)
	$(AS) -f elf64 $< -o $@

$(BUILD)/kernel.elf: $(OBJ)
	$(LD) -T kernel/linker.ld -o $@ $(OBJ) -nostdlib

initrd/%: usr/%.c
	$(USER_CC) $(USER_CFLAGS) $< $(USER_LDFLAGS) -o $@

$(BUILD)/initrd.tar: $(USER_PROGS)
	mkdir -p $(BUILD)
	tar -cf $(BUILD)/initrd.tar -C initrd .

iso: $(BUILD)/kernel.elf $(BUILD)/initrd.tar
	mkdir -p iso/boot/grub
	cp $(BUILD)/kernel.elf iso/boot/kernel.elf
	cp $(BUILD)/initrd.tar iso/boot/initrd.tar
	cp boot/grub/grub.cfg iso/boot/grub/grub.cfg
	grub-mkrescue -o myos.iso iso

run: all
	qemu-system-x86_64 -m 3G -cdrom myos.iso -serial file:kernel.log

debug: all
	qemu-system-x86_64 -m 3G -cdrom myos.iso -s -S -serial file:kernel.log

clean:
	rm -rf $(BUILD) iso myos.iso
	find initrd -maxdepth 1 -type f ! -name '*.tar' -delete

.PHONY: all iso run debug clean