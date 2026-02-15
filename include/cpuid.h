#ifndef CPUID_H
#define CPUID_H

#include "types.h"

typedef struct {
    char vendor[13];
    char brand[49];
    uint32_t family, model, stepping;
    bool has_fpu, has_sse, has_sse2, has_sse3, has_avx;
    bool has_pae, has_pse, has_apic, has_msr;
    uint32_t max_basic, max_extended;
} cpu_info_t;

void cpuid_init(void);
cpu_info_t* cpuid_get_info(void);
void cpuid_print(void);

#endif
