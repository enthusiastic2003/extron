#ifndef ELF_H
#define ELF_H
#include <stdint.h>


typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

typedef struct {
    Elf64_Word  sh_name;      /* Section name (index into string table) */
    Elf64_Word  sh_type;      /* Section type (e.g., SHT_PROGBITS) */
    Elf64_Xword sh_flags;     /* Section attributes (SHF_WRITE, SHF_ALLOC, etc.) */
    Elf64_Addr  sh_addr;      /* Virtual address in memory */
    Elf64_Off   sh_offset;    /* Offset in the file */
    Elf64_Xword sh_size;      /* Size of section in bytes */
    Elf64_Word  sh_link;      /* Link to another section */
    Elf64_Word  sh_info;      /* Additional section information */
    Elf64_Xword sh_addralign; /* Address alignment boundary */
    Elf64_Xword sh_entsize;   /* Size of entries, if section has a table */
} Elf64_Shdr;

/* Section Header Flags (sh_flags) */
#define SHF_WRITE            (1 << 0)   /* Writable */
#define SHF_ALLOC            (1 << 1)   /* Occupies memory during execution */
#define SHF_EXECINSTR        (1 << 2)   /* Executable instructions */
#define SHF_MASKPROC         0xF0000000 /* Processor-specific mask */

/* Section Types (sh_type) - Useful for debugging/filtering */
#define SHT_NULL             0
#define SHT_PROGBITS         1
#define SHT_SYMTAB           2
#define SHT_STRTAB           3
#define SHT_RELA             4
#define SHT_HASH             5
#define SHT_DYNAMIC          6
#define SHT_NOTE             7
#define SHT_NOBITS           8
#define SHT_REL              9
#define SHT_SHLIB            10
#define SHT_DYNSYM           11

#endif ELF_H