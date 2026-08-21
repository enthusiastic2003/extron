ARCH ?= x86_64

BUILD   = build

ifeq ($(ARCH),aarch64)

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

C_SRC   := $(shell find kernel/arch/aarch64 -name "*.c") \
           kernel/kernel.c \
           kernel/mm/pmm.c \
           kernel/console/console.c \
           kernel/panic.c \
           kernel/klibc/builtins.c
S_SRC   := $(shell find kernel/arch/aarch64 -name "*.S")
ASM_SRC :=
C_OBJ   := $(patsubst kernel/%.c,$(BUILD)/kernel/%.o,$(C_SRC))
S_OBJ   := $(patsubst kernel/%.S,$(BUILD)/kernel/%.o,$(S_SRC))
OBJ     := $(C_OBJ) $(S_OBJ)

KERNEL_ELF := $(BUILD)/kernel8.elf
KERNEL_IMG := $(BUILD)/kernel8.img

all: $(KERNEL_IMG)

$(BUILD)/kernel/%.o: kernel/%.S
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_ELF): $(OBJ) kernel/arch/aarch64/linker.ld
	$(LD) -T kernel/arch/aarch64/linker.ld -o $@ $(OBJ)

$(KERNEL_IMG): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

run: $(KERNEL_IMG)
	qemu-system-aarch64 -M raspi4b -serial stdio -display none -kernel $(KERNEL_IMG) -dtb boot/aarch64/bcm2711-rpi-4-b.dtb

else

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

C_SRC   := $(shell find kernel -name "*.c" -not -path "kernel/arch/aarch64/*")
ASM_SRC := $(shell find kernel -name "*.asm")
C_OBJ   := $(patsubst kernel/%.c,$(BUILD)/kernel/%.o,$(C_SRC))
ASM_OBJ := $(patsubst kernel/%.asm,$(BUILD)/kernel/%.o,$(ASM_SRC))
OBJ     := $(C_OBJ) $(ASM_OBJ)

USER_SRC   := $(wildcard usr/*.c)
USER_PROGS := $(patsubst usr/%.c,initrd/%,$(USER_SRC))

all: $(BUILD)/kernel.elf iso

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

endif

# Shared pattern rule: works for both arches since CC/CFLAGS switch above.
$(BUILD)/kernel/%.o: kernel/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD) iso myos.iso
	find initrd -maxdepth 1 -type f ! -name '*.tar' -delete

.PHONY: all iso run debug clean
