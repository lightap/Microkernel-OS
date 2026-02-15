#include "cpuid.h"
#include "vga.h"

static cpu_info_t cpu;

static inline void do_cpuid(uint32_t leaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
    __asm__ volatile ("cpuid" : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx) : "a"(leaf), "c"(0));
}

void cpuid_init(void) {
    uint32_t eax, ebx, ecx, edx;
    memset(&cpu, 0, sizeof(cpu));

    /* Vendor string */
    do_cpuid(0, &eax, &ebx, &ecx, &edx);
    cpu.max_basic = eax;
    memcpy(cpu.vendor + 0, &ebx, 4);
    memcpy(cpu.vendor + 4, &edx, 4);
    memcpy(cpu.vendor + 8, &ecx, 4);
    cpu.vendor[12] = '\0';

    /* Feature flags */
    if (cpu.max_basic >= 1) {
        do_cpuid(1, &eax, &ebx, &ecx, &edx);
        cpu.stepping = eax & 0xF;
        cpu.model    = (eax >> 4) & 0xF;
        cpu.family   = (eax >> 8) & 0xF;
        if (cpu.family == 6 || cpu.family == 15)
            cpu.model += ((eax >> 16) & 0xF) << 4;
        if (cpu.family == 15)
            cpu.family += (eax >> 20) & 0xFF;

        cpu.has_fpu  = (edx >> 0) & 1;
        cpu.has_pse  = (edx >> 3) & 1;
        cpu.has_pae  = (edx >> 6) & 1;
        cpu.has_msr  = (edx >> 5) & 1;
        cpu.has_apic = (edx >> 9) & 1;
        cpu.has_sse  = (edx >> 25) & 1;
        cpu.has_sse2 = (edx >> 26) & 1;
        cpu.has_sse3 = (ecx >> 0) & 1;
        cpu.has_avx  = (ecx >> 28) & 1;
    }

    /* Brand string */
    do_cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    cpu.max_extended = eax;
    if (eax >= 0x80000004) {
        uint32_t* brand = (uint32_t*)cpu.brand;
        do_cpuid(0x80000002, &brand[0], &brand[1], &brand[2], &brand[3]);
        do_cpuid(0x80000003, &brand[4], &brand[5], &brand[6], &brand[7]);
        do_cpuid(0x80000004, &brand[8], &brand[9], &brand[10], &brand[11]);
        cpu.brand[48] = '\0';
    } else {
        strcpy(cpu.brand, "Unknown");
    }
}

cpu_info_t* cpuid_get_info(void) { return &cpu; }

void cpuid_print(void) {
    /* Trim leading spaces from brand */
    const char* brand = cpu.brand;
    while (*brand == ' ') brand++;

    kprintf("  CPU:     %s\n", brand);
    kprintf("  Vendor:  %s\n", cpu.vendor);
    kprintf("  Family:  %u  Model: %u  Stepping: %u\n", cpu.family, cpu.model, cpu.stepping);
    kprintf("  Features:");
    if (cpu.has_fpu)  kprintf(" FPU");
    if (cpu.has_pse)  kprintf(" PSE");
    if (cpu.has_pae)  kprintf(" PAE");
    if (cpu.has_apic) kprintf(" APIC");
    if (cpu.has_msr)  kprintf(" MSR");
    if (cpu.has_sse)  kprintf(" SSE");
    if (cpu.has_sse2) kprintf(" SSE2");
    if (cpu.has_sse3) kprintf(" SSE3");
    if (cpu.has_avx)  kprintf(" AVX");
    kprintf("\n");
}
