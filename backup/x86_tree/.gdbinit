file build/kernel.elf
target remote localhost:1234

set disassemble-next-line on
set pagination off
set confirm off

set logging file regs.txt
set logging enabled on

add-symbol-file initrd/test

layout split
layout regs

break kmain