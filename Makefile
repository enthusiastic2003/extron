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

all: $(KERNEL_IMG)

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

run: $(KERNEL_IMG)
	qemu-system-aarch64 -M raspi4b -serial stdio -display none -kernel $(KERNEL_IMG) -dtb boot/bcm2711-rpi-4-b.dtb

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
