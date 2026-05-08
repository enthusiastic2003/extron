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
    Elf64_Word  sh_name;
    Elf64_Word  sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr  sh_addr;
    Elf64_Off   sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word  sh_link;
    Elf64_Word  sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
} Elf64_Shdr;
#define SHF_WRITE            (1 << 0)
#define SHF_ALLOC            (1 << 1)
#define SHF_EXECINSTR        (1 << 2)
#define SHF_MASKPROC         0xF0000000
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

/*  Additions below — existing code above is untouched  */

/* ELF magic / ident indexes */
#define EI_NIDENT   16
#define EI_MAG0     0
#define EI_MAG1     1
#define EI_MAG2     2
#define EI_MAG3     3
#define EI_CLASS    4
#define EI_DATA     5
#define ELFMAG0     0x7F
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'
#define ELFCLASS64  2
#define ELFDATA2LSB 1

/* File types */
#define ET_EXEC     2
#define ET_DYN      3

/* Machine */
#define EM_X86_64   62

/* ELF File Header */
typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half    e_type;
    Elf64_Half    e_machine;
    Elf64_Word    e_version;
    Elf64_Addr    e_entry;       /* Entry point */
    Elf64_Off     e_phoff;       /* Program header table offset */
    Elf64_Off     e_shoff;       /* Section header table offset */
    Elf64_Word    e_flags;
    Elf64_Half    e_ehsize;
    Elf64_Half    e_phentsize;
    Elf64_Half    e_phnum;       /* Number of program headers */
    Elf64_Half    e_shentsize;
    Elf64_Half    e_shnum;       /* Number of section headers */
    Elf64_Half    e_shstrndx;    /* Section name string table index */
} Elf64_Ehdr;

/* Program Header */
typedef struct {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;   /* Offset in file */
    Elf64_Addr  p_vaddr;    /* Virtual address */
    Elf64_Addr  p_paddr;    /* Physical address */
    Elf64_Xword p_filesz;   /* Size in file */
    Elf64_Xword p_memsz;    /* Size in memory (BSS padding included) */
    Elf64_Xword p_align;
} Elf64_Phdr;

/* Segment types */
#define PT_NULL     0
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_NOTE     4

/* Segment flags */
#define PF_X        (1 << 0)
#define PF_W        (1 << 1)
#define PF_R        (1 << 2)

/* Symbol Table Entry */
typedef struct {
    Elf64_Word    st_name;
    unsigned char st_info;
    unsigned char st_other;
    Elf64_Half    st_shndx;
    Elf64_Addr    st_value;
    Elf64_Xword   st_size;
} Elf64_Sym;

#define ELF64_ST_BIND(i)   ((i) >> 4)
#define ELF64_ST_TYPE(i)   ((i) & 0xF)
#define STB_LOCAL   0
#define STB_GLOBAL  1
#define STT_NOTYPE  0
#define STT_FUNC    2
#define STT_OBJECT  1

typedef enum {
    ELF_OK              = 0,
    ELF_ERR_NULL        = -1,  /* Null pointer passed */
    ELF_ERR_TOO_SMALL   = -2,  /* Buffer smaller than ELF header */
    ELF_ERR_MAGIC       = -3,  /* Bad magic bytes */
    ELF_ERR_CLASS       = -4,  /* Not 64-bit */
    ELF_ERR_ENDIAN      = -5,  /* Not little-endian */
    ELF_ERR_MACHINE     = -6,  /* Not x86_64 */
    ELF_ERR_TYPE        = -7,  /* Not executable or shared object */
    ELF_ERR_PHOFF       = -8,  /* Program header table out of bounds */
    ELF_ERR_PHENTSIZE   = -9,  /* Program header entry size mismatch */
    ELF_ERR_ENTRY       = -10, /* Entry point is null */
    ELF_ERR_BASE        = -11
} Elf64_ValidationResult;


/*Extron Specific User Program Load Constants*/
#define ELF_USER_EXPECTED_BASE 0x400000
#endif /* ELF_H */