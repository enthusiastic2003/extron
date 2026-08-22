#!/bin/bash

# Extron OS Cross-Compiler Toolchain
EXTRON_TOOLCHAIN="/home/sirjanh/extron-toolkit/toolchain/bin"

# Freestanding x86_64-elf Toolchain
FREESTANDING_TOOLCHAIN="/home/sirjanh/x86_64-elf-tools-linux/bin"

# Update PATH
export PATH="$EXTRON_TOOLCHAIN:$FREESTANDING_TOOLCHAIN:$PATH"

echo "Cross-compiler environment set up."
echo "  Extron GCC: $(which x86_64-extron-gcc 2>/dev/null || echo 'NOT FOUND')"
echo "  ELF GCC:    $(which x86_64-elf-gcc 2>/dev/null || echo 'NOT FOUND')"
