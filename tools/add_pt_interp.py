#!/usr/bin/env python3
"""
add_pt_interp.py IN_ELF INTERP_PATH OUT_ELF

Splices a PT_INTERP program header onto an existing, already-linked
static ET_EXEC ELF64/little-endian binary, without disturbing any of
its existing content or offsets — used to build a PT_INTERP regression
fixture (see usr/mlibc_tests/mlibc_ptinterp_victim.c) since this
project's toolchain has no real dynamic linker to link against yet, so
there's no ordinary way to get a real PT_INTERP segment out of it.

Approach: every existing byte of the input file stays exactly where it
is — this only APPENDS two things past the end of the file: the
INTERP_PATH string, and a brand-new program header table (a copy of
the original entries plus one new PT_INTERP entry) — then repoints
e_phoff/e_phnum at that new table. Nothing already in the file (any
existing p_offset/p_vaddr, or the ELF header's own e_entry) needs to
change, since nothing already there moved.
"""
import os
import struct
import sys

EHDR_E_PHOFF_OFF   = 0x20  # Elf64_Ehdr.e_phoff  (Elf64_Off,  8 bytes)
EHDR_E_PHENTSIZE_OFF = 0x36  # Elf64_Ehdr.e_phentsize (Elf64_Half, 2 bytes)
EHDR_E_PHNUM_OFF   = 0x38  # Elf64_Ehdr.e_phnum  (Elf64_Half, 2 bytes)

PT_INTERP = 3
PF_R      = 4

PHDR_FORMAT = "<IIQQQQQQ"  # p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align
PHDR_SIZE   = struct.calcsize(PHDR_FORMAT)
assert PHDR_SIZE == 56, "Elf64_Phdr must be 56 bytes"


def main():
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} IN_ELF INTERP_PATH OUT_ELF", file=sys.stderr)
        return 1
    in_path, interp_path, out_path = sys.argv[1:4]

    with open(in_path, "rb") as f:
        data = bytearray(f.read())

    if data[0:4] != b"\x7fELF":
        print(f"{in_path}: not an ELF file", file=sys.stderr)
        return 1

    e_phoff, = struct.unpack_from("<Q", data, EHDR_E_PHOFF_OFF)
    e_phentsize, = struct.unpack_from("<H", data, EHDR_E_PHENTSIZE_OFF)
    e_phnum, = struct.unpack_from("<H", data, EHDR_E_PHNUM_OFF)
    if e_phentsize != PHDR_SIZE:
        print(f"{in_path}: unexpected e_phentsize {e_phentsize}, expected {PHDR_SIZE}",
              file=sys.stderr)
        return 1

    old_table = bytes(data[e_phoff:e_phoff + e_phnum * e_phentsize])

    # Append the interpreter path string.
    interp_bytes = interp_path.encode("utf-8") + b"\x00"
    interp_str_off = len(data)
    data += interp_bytes

    new_phdr = struct.pack(
        PHDR_FORMAT,
        PT_INTERP,
        PF_R,
        interp_str_off,      # p_offset
        0,                   # p_vaddr  (unused for PT_INTERP)
        0,                   # p_paddr
        len(interp_bytes),   # p_filesz
        len(interp_bytes),   # p_memsz
        1,                   # p_align
    )

    # New, contiguous program header table: the untouched original
    # entries plus the new one, placed after the interp string.
    new_table_off = len(data)
    data += old_table + new_phdr

    struct.pack_into("<Q", data, EHDR_E_PHOFF_OFF, new_table_off)
    struct.pack_into("<H", data, EHDR_E_PHNUM_OFF, e_phnum + 1)

    with open(out_path, "wb") as f:
        f.write(data)
    # open(..., "wb") ignores the input's mode entirely, so a freshly
    # compiled (executable) input would otherwise produce a non-executable
    # output — carry the executable bits across explicitly.
    os.chmod(out_path, os.stat(in_path).st_mode & 0o777)
    return 0


if __name__ == "__main__":
    sys.exit(main())
