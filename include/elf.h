#ifndef ELF_H
#define ELF_H

#include "types.h"

/*
 * ELF32 binary loader for the microkernel.
 *
 * Loads user-space ELF binaries into isolated address spaces.
 * Each server/process gets its own page directory where:
 *   - Kernel pages (0-1GB) are mapped supervisor-only (ring 3 can't touch them)
 *   - User code/data are mapped at 0x40000000+ with PAGE_USER
 *   - User stack is mapped at USER_STACK_TOP growing downward
 *
 * This gives true memory isolation: a buggy server cannot corrupt
 * the kernel or other processes.
 */

/* ELF magic bytes */
#define ELF_MAGIC       0x464C457F  /* "\x7FELF" as uint32_t */

/* ELF identification indices */
#define EI_CLASS        4
#define EI_DATA         5
#define ELFCLASS32      1       /* 32-bit ELF */
#define ELFDATA2LSB     1       /* Little-endian */

/* ELF types */
#define ET_EXEC         2       /* Executable */

/* ELF machine types */
#define EM_386          3       /* x86 */

/* Program header types */
#define PT_NULL         0
#define PT_LOAD         1       /* Loadable segment */

/* Program header flags */
#define PF_X            0x1     /* Execute */
#define PF_W            0x2     /* Write */
#define PF_R            0x4     /* Read */

/* ---- ELF32 structures ---- */

typedef struct {
    uint32_t e_ident_magic;         /* 0x7F 'E' 'L' 'F' */
    uint8_t  e_ident_class;         /* ELFCLASS32 */
    uint8_t  e_ident_data;          /* ELFDATA2LSB */
    uint8_t  e_ident_version;
    uint8_t  e_ident_osabi;
    uint8_t  e_ident_pad[8];
    uint16_t e_type;                /* ET_EXEC */
    uint16_t e_machine;             /* EM_386 */
    uint32_t e_version;
    uint32_t e_entry;               /* Entry point virtual address */
    uint32_t e_phoff;               /* Program header table offset */
    uint32_t e_shoff;               /* Section header table offset */
    uint32_t e_flags;
    uint16_t e_ehsize;              /* ELF header size */
    uint16_t e_phentsize;           /* Program header entry size */
    uint16_t e_phnum;               /* Number of program headers */
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf32_ehdr_t;

typedef struct {
    uint32_t p_type;                /* PT_LOAD = 1 */
    uint32_t p_offset;              /* Offset in file */
    uint32_t p_vaddr;               /* Virtual address in memory */
    uint32_t p_paddr;               /* Physical address (unused) */
    uint32_t p_filesz;              /* Size of segment in file */
    uint32_t p_memsz;               /* Size of segment in memory (>= filesz) */
    uint32_t p_flags;               /* PF_R | PF_W | PF_X */
    uint32_t p_align;               /* Alignment */
} __attribute__((packed)) elf32_phdr_t;

/* ---- User address space layout ---- */
#define USER_BASE       0xC0000000  /* Move to 3GB mark */
#define USER_STACK_TOP  0xE0000000  /* Move stack much higher */
#define USER_STACK_PAGES 4          /* 16KB user stack */
#define USER_VGA_VADDR  0xB0000000  /* VGA memory mapped here for console server */

/* Physical address of VGA text buffer */
#define VGA_PHYS_BASE   0xB8000

/* ---- ELF loading flags ---- */

#define ELF_MAP_VGA     (1 << 0)    /* Map VGA framebuffer into address space */

/* ---- API ---- */

/*
 * Validate an ELF binary in memory.
 * Returns 0 if valid, -1 if not.
 */
int elf_validate(const void* data, uint32_t size);

/*
 * Load an ELF binary and create a new task for it.
 *
 * data:          Pointer to ELF binary in kernel memory
 * size:          Size of ELF binary in bytes
 * name:          Task name (for ps/debugging)
 * priority:      Scheduler priority
 * io_privileged: If true, set IOPL=3 for port I/O access
 * flags:         ELF_MAP_VGA etc.
 *
 * Returns PID on success, -1 on failure.
 */
int32_t elf_load(const void* data, uint32_t size, const char* name,
                 uint32_t priority, bool io_privileged, uint32_t flags);

#endif
