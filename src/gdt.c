#include "gdt.h"
#include "vga.h"

extern void gdt_flush(uint32_t);
extern void tss_flush(void);

/* 6 entries: null, kcode, kdata, ucode, udata, tss */
static struct gdt_entry gdt_entries[6];
static struct gdt_ptr   gdt_pointer;
static tss_t            tss;

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt_entries[num].base_low    = (base & 0xFFFF);
    gdt_entries[num].base_middle = (base >> 16) & 0xFF;
    gdt_entries[num].base_high   = (base >> 24) & 0xFF;
    gdt_entries[num].limit_low   = (limit & 0xFFFF);
    gdt_entries[num].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt_entries[num].access      = access;
}

void gdt_init(void) {
    gdt_pointer.limit = (sizeof(struct gdt_entry) * 6) - 1;
    gdt_pointer.base  = (uint32_t)&gdt_entries;

    gdt_set_gate(0, 0, 0,          0,    0);           /* NULL */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);        /* Kernel code */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);        /* Kernel data */
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);        /* User code   */
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);        /* User data   */

    /* TSS entry — type 0x89 = present | ring 0 | TSS available (not busy) */
    memset(&tss, 0, sizeof(tss));
    tss.ss0  = SEG_KDATA;          /* Kernel data segment for ring transitions */
    tss.esp0 = 0;                  /* Will be set per-task on context switch */
    tss.cs   = SEG_KCODE | 3;     /* Kernel code with RPL bits for iret */
    tss.ss   = SEG_KDATA | 3;
    tss.ds   = SEG_KDATA | 3;
    tss.es   = SEG_KDATA | 3;
    tss.fs   = SEG_KDATA | 3;
    tss.gs   = SEG_KDATA | 3;
    tss.iomap_base = sizeof(tss);  /* No I/O bitmap — IOPL controls access */

    uint32_t tss_base  = (uint32_t)&tss;
    uint32_t tss_limit = sizeof(tss) - 1;
    gdt_set_gate(5, tss_base, tss_limit, 0x89, 0x00);

    gdt_flush((uint32_t)&gdt_pointer);
    tss_flush();
}

void tss_set_kernel_stack(uint32_t esp0) {
    tss.esp0 = esp0;
}
