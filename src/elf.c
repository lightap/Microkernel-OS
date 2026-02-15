#include "elf.h"
#include "paging.h"
#include "pmm.h"
#include "task.h"
#include "heap.h"
#include "gdt.h"
#include "vga.h"
#include "serial.h"

/*
 * ELF Loader — loads user-space binaries into isolated address spaces.
 *
 * This is the key piece that makes the microkernel real: each server
 * runs in its own address space where it CANNOT access kernel memory
 * or other processes' memory. If a server crashes, it can't take
 * down the kernel.
 *
 * Loading process:
 *   1. Validate ELF header (32-bit, x86, executable)
 *   2. Create isolated page directory (kernel pages supervisor-only)
 *   3. For each PT_LOAD segment:
 *      a. Allocate physical frames
 *      b. Copy file data, zero BSS
 *      c. Map at segment's virtual address with PAGE_USER
 *   4. Allocate and map user stack
 *   5. Allocate kernel stack (for ring 3 → ring 0 transitions)
 *   6. Create task with entry point from ELF header
 */

int elf_validate(const void* data, uint32_t size) {
    if (!data || size < sizeof(elf32_ehdr_t))
        return -1;

    const elf32_ehdr_t* ehdr = (const elf32_ehdr_t*)data;

    /* Check ELF magic */
    if (ehdr->e_ident_magic != ELF_MAGIC) {
        serial_printf("ELF: bad magic %x\n", ehdr->e_ident_magic);
        return -1;
    }

    /* Must be 32-bit little-endian executable for i386 */
    if (ehdr->e_ident_class != ELFCLASS32 ||
        ehdr->e_ident_data != ELFDATA2LSB ||
        ehdr->e_type != ET_EXEC ||
        ehdr->e_machine != EM_386) {
        serial_printf("ELF: not a 32-bit x86 executable\n");
        return -1;
    }

    /* Must have program headers */
    if (ehdr->e_phnum == 0 || ehdr->e_phoff == 0) {
        serial_printf("ELF: no program headers\n");
        return -1;
    }

    /* Program headers must be within the file */
    uint32_t ph_end = ehdr->e_phoff + (uint32_t)ehdr->e_phnum * ehdr->e_phentsize;
    if (ph_end > size) {
        serial_printf("ELF: program headers outside file\n");
        return -1;
    }

    /* Entry point must be in user space */
    if (ehdr->e_entry < USER_BASE) {
        serial_printf("ELF: entry point %x below user base %x\n",
                      ehdr->e_entry, USER_BASE);
        return -1;
    }

    return 0;
}

/*
 * Map an ELF segment into a user address space.
 *
 * Allocates physical pages, copies data from the ELF file,
 * zeros BSS (memsz > filesz), and maps with appropriate permissions.
 */
// src/elf.c

static int map_segment(uint32_t* pd, const elf32_phdr_t* phdr, const uint8_t* file_data) {
    uint32_t vaddr_start = phdr->p_vaddr & ~0xFFF;
    uint32_t vaddr_end   = (phdr->p_vaddr + phdr->p_memsz + 0xFFF) & ~0xFFF;
    uint32_t num_pages   = (vaddr_end - vaddr_start) / PAGE_SIZE;

    uint32_t flags = PAGE_PRESENT | PAGE_USER;
    if (phdr->p_flags & PF_W) flags |= PAGE_WRITE;

    for (uint32_t i = 0; i < num_pages; i++) {
        uint32_t page_vaddr = vaddr_start + i * PAGE_SIZE;

        // Allocate frame
        uint32_t frame_phys = (uint32_t)pmm_alloc_page();
        if (!frame_phys) return -1;

        // Identity mapped kernel: write directly to frame address
        memset((void*)frame_phys, 0, PAGE_SIZE);

        // Copy file data if it falls within this page
        uint32_t seg_file_start = phdr->p_vaddr;
        uint32_t seg_file_end   = phdr->p_vaddr + phdr->p_filesz;
        uint32_t page_start = page_vaddr;
        uint32_t page_end   = page_vaddr + PAGE_SIZE;

        if (seg_file_end > page_start && seg_file_start < page_end) {
            uint32_t copy_start = (seg_file_start > page_start) ? seg_file_start : page_start;
            uint32_t copy_end   = (seg_file_end < page_end) ? seg_file_end : page_end;
            uint32_t copy_len   = copy_end - copy_start;

            uint32_t file_offset = phdr->p_offset + (copy_start - phdr->p_vaddr);
            uint32_t page_offset = copy_start - page_start;

            memcpy((uint8_t*)frame_phys + page_offset, file_data + file_offset, copy_len);
        }

        // Map into user PD
        paging_map_user(pd, page_vaddr, frame_phys, flags);
    }
    return 0;
}



int32_t elf_load(const void* data, uint32_t size, const char* name,
                 uint32_t priority, bool io_privileged, uint32_t flags) {
    /* Validate first */
    if (elf_validate(data, size) != 0)
        return -1;

    const elf32_ehdr_t* ehdr = (const elf32_ehdr_t*)data;
    const uint8_t* file_data = (const uint8_t*)data;

    serial_printf("ELF: loading '%s' entry=%x phnum=%u\n",
                  name, ehdr->e_entry, ehdr->e_phnum);

    /* Create an isolated address space.
     * Kernel pages are supervisor-only — ring 3 code CANNOT access them. */
    uint32_t* pd = paging_create_isolated_space();
    if (!pd) {
        serial_printf("ELF: failed to create address space\n");
        return -1;
    }

    /* Load each PT_LOAD segment */
    for (uint32_t i = 0; i < ehdr->e_phnum; i++) {
        const elf32_phdr_t* phdr = (const elf32_phdr_t*)
            (file_data + ehdr->e_phoff + i * ehdr->e_phentsize);

        if (phdr->p_type != PT_LOAD)
            continue;

        if (phdr->p_memsz == 0)
            continue;

        /* Validate segment is in user space */
        if (phdr->p_vaddr < USER_BASE) {
            serial_printf("ELF: segment at %x below user base\n", phdr->p_vaddr);
            paging_destroy_address_space(pd);
            return -1;
        }

        /* Validate file data is within bounds */
        if (phdr->p_offset + phdr->p_filesz > size) {
            serial_printf("ELF: segment file data exceeds binary size\n");
            paging_destroy_address_space(pd);
            return -1;
        }

        serial_printf("ELF:   LOAD vaddr=%x memsz=%x filesz=%x flags=%x\n",
                      phdr->p_vaddr, phdr->p_memsz, phdr->p_filesz, phdr->p_flags);

        if (map_segment(pd, phdr, file_data) != 0) {
            paging_destroy_address_space(pd);
            return -1;
        }
    }

    /* Allocate and map user stack (grows downward from USER_STACK_TOP) */
/* Allocate and map user stack (grows downward from USER_STACK_TOP) */
    uint32_t stack_bottom = USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE);
    const uint32_t TEMP_MAP_VA = 0x3FD00000;
    
    for (uint32_t i = 0; i < USER_STACK_PAGES; i++) {
        uint32_t frame_phys = (uint32_t)pmm_alloc_page();
        if (!frame_phys) {
            serial_printf("ELF: out of memory for user stack\n");
            paging_destroy_address_space(pd);
            return -1;
        }
        
        /* Zero the stack page using temporary mapping */
        paging_map_page(TEMP_MAP_VA, frame_phys, PAGE_PRESENT | PAGE_WRITE);
        memset((void*)TEMP_MAP_VA, 0, PAGE_SIZE);
        paging_unmap_page(TEMP_MAP_VA);
        
        /* Map into user space */
        paging_map_user(pd, stack_bottom + i * PAGE_SIZE, frame_phys,
                        PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
    }

    /* Map VGA memory if requested (for console server) */
    if (flags & ELF_MAP_VGA) {
        /* Map 2 pages of VGA text buffer (physical 0xB8000) at USER_VGA_VADDR */
        paging_map_user(pd, USER_VGA_VADDR, VGA_PHYS_BASE,
                        PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
        paging_map_user(pd, USER_VGA_VADDR + PAGE_SIZE, VGA_PHYS_BASE + PAGE_SIZE,
                        PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
        serial_printf("ELF:   mapped VGA at %x -> phys %x\n",
                      USER_VGA_VADDR, VGA_PHYS_BASE);
    }

    /* Allocate kernel stack (for ring 3 → ring 0 transitions on interrupts) */
    void* kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!kernel_stack) {
        serial_printf("ELF: out of memory for kernel stack\n");
        paging_destroy_address_space(pd);
        return -1;
    }

    /* Create the task using the ELF-specific task creation function */
    int32_t pid = task_create_from_elf(
        name,
        ehdr->e_entry,           /* Entry point from ELF header */
        USER_STACK_TOP,          /* User stack top */
        pd,                      /* Isolated page directory */
        (uint32_t)kernel_stack,  /* Kernel stack base */
        KERNEL_STACK_SIZE,       /* Kernel stack size */
        priority,
        io_privileged
    );

    if (pid < 0) {
        serial_printf("ELF: failed to create task\n");
        kfree(kernel_stack);
        paging_destroy_address_space(pd);
        return -1;
    }

    serial_printf("ELF: loaded '%s' as PID %u (entry=%x, isolated address space)\n",
                  name, pid, ehdr->e_entry);
    return pid;
}
