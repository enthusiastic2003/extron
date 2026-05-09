file build/kernel.elf
target remote localhost:1234

set disassemble-next-line on
set pagination off
set confirm off

set logging file regs.txt
set logging enabled on

layout split
layout regs

break kmain