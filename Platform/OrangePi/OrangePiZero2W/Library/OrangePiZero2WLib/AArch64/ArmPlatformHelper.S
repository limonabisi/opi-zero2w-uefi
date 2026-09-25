//
//  Orange Pi Zero 2W - very first code executed by EDK2 (called from the
//  PeilessSec entry point, before stack/C setup).
//
//  Prints "\r\n<EDK2 ELn>\r\n" straight to UART0 so that a UART log shows
//  whether BL31 handed over to us, even if something later hangs.
//  Uses only x9-x13; x0-x3 (boot arguments) and x30 are preserved.
//
//  SPDX-License-Identifier: BSD-2-Clause-Patent
//

#include <AsmMacroLib.h>

#define UART0_BASE  0x05000000
#define UART_LSR    0x14
#define LSR_THRE    (1 << 5)

// putc: w12 = character. Bounded wait on THRE.
.macro PUTC_W12
  mov   x11, #0x100000
1:
  ldr   w10, [x9, #UART_LSR]
  tbnz  w10, #5, 2f
  subs  x11, x11, #1
  b.ne  1b
2:
  str   w12, [x9]
.endm

.macro PUTC ch
  mov   w12, #\ch
  PUTC_W12
.endm

ASM_FUNC(ArmPlatformPeiBootAction)
  MOV32 (x9, UART0_BASE)
  PUTC  0x0d
  PUTC  0x0a
  PUTC  '<'
  PUTC  'E'
  PUTC  'D'
  PUTC  'K'
  PUTC  '2'
  PUTC  ' '
  PUTC  'E'
  PUTC  'L'
  mrs   x13, CurrentEL
  ubfx  x13, x13, #2, #2
  add   w12, w13, #'0'
  PUTC_W12
  PUTC  '>'
  PUTC  0x0d
  PUTC  0x0a
  ret
