/*
 * x64 Engine: run the firmware (and the engine) at EL1 while the x86
 * machine runs, so that the EL1&0 translation regime - with a second
 * table base, TTBR1 - is available to map the x86 address space directly.
 *
 * The firmware itself runs at EL2 with an identity map. Going down to EL1
 * keeps everything working as before: TTBR0_EL1 uses the very same tables
 * (the stage 1 descriptor formats of EL2 and EL1&0 are compatible), the
 * firmware's exception vectors work at either level, and its timer is the
 * EL1 physical timer. Interrupts are taken at EL1 (HCR_EL2.IMO = 0).
 *
 * Coming back up is an HVC, caught by a tiny EL2 vector table that resumes
 * the caller at EL2 on the same stack.
 */
#include <stdint.h>
#include "x64e_el.h"

__asm__ (
  "  .text\n"
  "  .balign 2048\n"
  "  .global x64e_el2_vectors\n"
  "x64e_el2_vectors:\n"
  /* current EL with SP0 / SPx: not expected while we run at EL1 */
  "  .rept 8\n"
  "  b .\n"
  "  .balign 0x80\n"
  "  .endr\n"
  /* lower EL, AArch64: synchronous = our HVC */
  "  b x64e_el2_lower_sync\n"
  "  .balign 0x80\n"
  "  .rept 7\n"
  "  b .\n"
  "  .balign 0x80\n"
  "  .endr\n"
  "\n"
  "x64e_el2_lower_sync:\n"
  "  mrs   x9, esr_el2\n"
  "  lsr   x9, x9, #26\n"
  "  cmp   x9, #0x16\n"            /* EC = HVC from AArch64 */
  "  b.ne  .\n"
  "  mrs   x9, sp_el1\n"
  "  mov   sp, x9\n"               /* continue on the same stack */
  "  mrs   x9, spsr_el2\n"
  "  bic   x9, x9, #0xf\n"
  "  mov   x10, #0x9\n"            /* EL2h */
  "  orr   x9, x9, x10\n"
  "  msr   spsr_el2, x9\n"
  "  eret\n"                       /* to the instruction after the HVC */
  "\n"
  /* void x64e_enter_el1 (uint64_t el2_vectors, uint64_t tcr_el1_extra) */
  "  .balign 8\n"
  "  .global x64e_enter_el1\n"
  "x64e_enter_el1:\n"
  "  mrs   x2, daif\n"
  "  msr   daifset, #0xf\n"
  /* firmware vectors serve EL1 too */
  "  mrs   x3, vbar_el2\n"
  "  msr   vbar_el1, x3\n"
  "  msr   vbar_el2, x0\n"
  /* memory attributes and the identity map: same as EL2 */
  "  mrs   x3, mair_el2\n"
  "  msr   mair_el1, x3\n"
  "  mrs   x3, ttbr0_el2\n"
  "  msr   ttbr0_el1, x3\n"
  /* TCR_EL1: TTBR0 part from TCR_EL2, PS -> IPS, TTBR1 off (EPD1) for now */
  "  mrs   x3, tcr_el2\n"
  "  and   x4, x3, #0xffff\n"
  "  ubfx  x5, x3, #16, #3\n"
  "  orr   x4, x4, x5, lsl #32\n"
  "  orr   x4, x4, x1\n"
  "  msr   tcr_el1, x4\n"
  /* SCTLR_EL1: RES1 bits + M, A, C, SA, I, EE of SCTLR_EL2 */
  "  mrs   x3, sctlr_el2\n"
  "  mov   x4, #0x100f\n"
  "  movk  x4, #0x0200, lsl #16\n"
  "  and   x3, x3, x4\n"
  "  mov   x4, #0x0800\n"
  "  movk  x4, #0x30d0, lsl #16\n"
  "  orr   x3, x3, x4\n"
  "  orr   x3, x3, #0x10000\n"     /* nTWI */
  "  orr   x3, x3, #0x40000\n"     /* nTWE */
  "  msr   sctlr_el1, x3\n"
  /* FP/SIMD at EL1, no traps from EL2 */
  "  mov   x3, #(3 << 20)\n"
  "  msr   cpacr_el1, x3\n"
  "  mov   x3, #0x33ff\n"
  "  msr   cptr_el2, x3\n"
  /* EL1 may use the physical counter and timer */
  "  mrs   x3, cnthctl_el2\n"
  "  orr   x3, x3, #3\n"
  "  msr   cnthctl_el2, x3\n"
  "  msr   cntvoff_el2, xzr\n"
  /* EL1 is AArch64, no stage 2, interrupts stay at EL1 */
  "  mov   x3, #(1 << 31)\n"
  "  msr   hcr_el2, x3\n"
  "  isb\n"
  /* drop to EL1h on the current stack, interrupt mask as the caller's */
  "  mov   x3, sp\n"
  "  msr   sp_el1, x3\n"
  "  adr   x3, 1f\n"
  "  msr   elr_el2, x3\n"
  "  and   x3, x2, #0x3c0\n"
  "  mov   x4, #0x5\n"             /* EL1h */
  "  orr   x3, x3, x4\n"
  "  msr   spsr_el2, x3\n"
  "  eret\n"
  "1:\n"
  "  tlbi  vmalle1\n"
  "  dsb   nsh\n"
  "  isb\n"
  "  ret\n"
  "\n"
  /* void x64e_leave_el1 (uint64_t firmware_el2_vectors, uint64_t firmware_hcr_el2) */
  "  .balign 8\n"
  "  .global x64e_leave_el1\n"
  "x64e_leave_el1:\n"
  "  mrs   x2, daif\n"
  "  msr   daifset, #0xf\n"
  "  hvc   #0x64\n"
  /* back at EL2: the firmware's vectors and HCR (it sets TGE) */
  "  msr   vbar_el2, x0\n"
  "  msr   hcr_el2, x1\n"
  "  isb\n"
  "  tlbi  alle2\n"
  "  dsb   nsh\n"
  "  isb\n"
  "  msr   daif, x2\n"
  "  ret\n"
);

uint64_t
x64e_current_el (void)
{
  uint64_t el;

  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (el));
  return (el >> 2) & 3;
}

uint64_t
x64e_read_hcr_el2 (void)
{
  uint64_t v;

  __asm__ volatile ("mrs %0, hcr_el2" : "=r" (v));
  return v;
}

uint64_t
x64e_read_vbar_el2 (void)
{
  uint64_t v;

  __asm__ volatile ("mrs %0, vbar_el2" : "=r" (v));
  return v;
}
