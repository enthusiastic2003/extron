AS      = nasm
CC      = x86_64-elf-gcc
LD      = x86_64-elf-ld

CFLAGS  = -ffreestanding -O2 -Wall -Wextra -mcmodel=large \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -nostdlib \
          -Ikernel/include -Ikernel/arch/x86_64/include

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
iso: $(BUILD)/kernel.elf
	mkdir -p iso/boot/grub
	cp $(BUILD)/kernel.elf iso/boot/kernel.elf
	cp boot/grub/grub.cfg iso/boot/grub/grub.cfg
	grub-mkrescue -o myos.iso iso

# --- Run (QEMU helper) ---
run: all
	qemu-system-x86_64 -cdrom myos.iso

# --- Debug (QEMU + GDB stub) ---
debug: all
	qemu-system-x86_64 -cdrom myos.iso -s -S

# --- Clean ---
clean:
	rm -rf $(BUILD) iso myos.iso

.PHONY: all iso run debug clean