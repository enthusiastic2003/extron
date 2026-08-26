#!/usr/bin/env python3
"""
add_pt_interp.py IN_ELF INTERP_PATH OUT_ELF

Splices a PT_INTERP program header onto an existing, already-linked
static ET_EXEC ELF64/little-endian binary, without disturbing any of
its existing content or offsets. This legacy fixture remains useful as
a kernel handoff test alongside the real mlibc dynamic linker.

The new header is placed in the linker's zero padding immediately after
the existing program-header table, keeping AT_PHDR inside the first
PT_LOAD as required by real dynamic linkers. Only the interpreter string
is appended to the file.
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

    new_header_off = e_phoff + e_phnum * e_phentsize
    new_header_end = new_header_off + PHDR_SIZE
    containing_load = False
    for i in range(e_phnum):
        ph = struct.unpack_from(PHDR_FORMAT, data, e_phoff + i * e_phentsize)
        p_type, p_offset, p_filesz = ph[0], ph[2], ph[5]
        if p_type == 1 and new_header_off >= p_offset and new_header_end <= p_offset + p_filesz:
            containing_load = True
            break
    if not containing_load or any(data[new_header_off:new_header_end]):
        print(f"{in_path}: no zero PT_LOAD padding for another program header",
              file=sys.stderr)
        return 1
    data[new_header_off:new_header_end] = new_phdr
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
