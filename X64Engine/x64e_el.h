/* x64 Engine: run at EL1 while the x86 machine runs (AArch64 firmware only) */
#ifndef X64E_EL_H
#define X64E_EL_H

#include <stdint.h>

extern char x64e_el2_vectors[];

uint64_t x64e_current_el (void);
uint64_t x64e_read_vbar_el2 (void);
void     x64e_enter_el1 (uint64_t el2_vectors, uint64_t tcr_el1_extra);
void     x64e_leave_el1 (uint64_t firmware_el2_vectors, uint64_t firmware_hcr_el2);
uint64_t x64e_read_hcr_el2 (void);

/* TCR_EL1 bits for TTBR1: EPD1 = no walks yet */
#define X64E_TCR_EPD1  (1ULL << 23)

#endif
