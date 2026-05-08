file build/kernel.elf
target remote localhost:1234

set disassemble-next-line on
set pagination off
set confirm off

layout split
layout regs

break kmain