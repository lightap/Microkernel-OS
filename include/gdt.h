#ifndef GDT_H
#define GDT_H

#include "types.h"

/* Segment selectors */
#define SEG_KCODE   0x08    /* Kernel code: GDT entry 1 */
#define SEG_KDATA   0x10    /* Kernel data: GDT entry 2 */
#define SEG_UCODE   0x1B    /* User code:   GDT entry 3 | RPL 3 */
#define SEG_UDATA   0x23    /* User data:   GDT entry 4 | RPL 3 */
#define SEG_TSS     0x28    /* TSS:         GDT entry 5 */

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* Task State Segment — required for ring 3 → ring 0 transitions */
typedef struct __attribute__((packed)) {
    uint32_t prev_tss;
    uint32_t esp0;          /* Kernel stack pointer (loaded on ring transition) */
    uint32_t ss0;           /* Kernel stack segment (loaded on ring transition) */
    uint32_t esp1, ss1;
    uint32_t esp2, ss2;
    uint32_t cr3;
    uint32_t eip, eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} tss_t;

void gdt_init(void);

/* Set the kernel stack in the TSS — must be called on every context switch
 * to a ring 3 task so the CPU knows where to switch the stack on interrupts */
void tss_set_kernel_stack(uint32_t esp0);

#endif
