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

// Green status LED: PC13 (PIO bank C = 0x0300B048)
#define PIO_PC_CFG1  0x0300B04C   // PC8..PC15, 4 bits each
#define PIO_PC_DAT   0x0300B058

ASM_FUNC(ArmPlatformPeiBootAction)
  // LED on as the very first thing: visible proof that EDK2 is running,
  // independent of the UART.
  MOV32 (x9, PIO_PC_CFG1)
  ldr   w10, [x9]
  bic   w10, w10, #(0xF << 20)
  orr   w10, w10, #(0x1 << 20)      // PC13 = output
  str   w10, [x9]
  MOV32 (x9, PIO_PC_DAT)
  ldr   w10, [x9]
  orr   w10, w10, #(1 << 13)        // PC13 = high -> LED on
  str   w10, [x9]

  //
  // Re-initialise UART0 from scratch, exactly like U-Boot proper does
  // (clock source, gate/reset, pin mux, divisor). Something between SPL
  // and here leaves the UART in a bad state; U-Boot hides that because it
  // re-inits the console, so we do the same.
  //
  MOV32 (x9, 0x03001524)            // CCU APB2_CFG: OSC24M, N=1, M=1 -> 24 MHz
  str   wzr, [x9]
  MOV32 (x9, 0x0300190C)            // CCU UART_BGR: UART0 gate + de-assert reset
  ldr   w10, [x9]
  orr   w10, w10, #(1 << 0)
  orr   w10, w10, #(1 << 16)
  str   w10, [x9]
  MOV32 (x9, 0x0300B0FC)            // PIO bank H CFG0: PH0/PH1 = function 2 (UART0)
  ldr   w10, [x9]
  bic   w10, w10, #0xFF
  mov   w11, #0x22
  orr   w10, w10, w11
  str   w10, [x9]
  MOV32 (x9, 0x0300B118)            // PIO bank H PULL0: PH1 pull-up
  ldr   w10, [x9]
  bic   w10, w10, #(0x3 << 2)
  orr   w10, w10, #(0x1 << 2)
  str   w10, [x9]

  MOV32 (x9, UART0_BASE)
  // wait (bounded) for the transmitter to drain what BL31 printed
  mov   x11, #0x100000
3:
  ldr   w10, [x9, #UART_LSR]
  tbnz  w10, #6, 4f                 // TEMT
  subs  x11, x11, #1
  b.ne  3b
4:
  str   wzr, [x9, #0x04]            // IER = 0
  mov   w10, #0x02
  str   w10, [x9, #0xA4]            // HALT: CHCFG_AT_BUSY (allow LCR/DLL change while busy)
  mov   w10, #0x83
  str   w10, [x9, #0x0C]            // LCR: DLAB | 8N1
  mov   w10, #13
  str   w10, [x9, #0x00]            // DLL = 13 -> 24 MHz / (16 * 13) = 115384 baud
  str   wzr, [x9, #0x04]            // DLH = 0
  mov   w10, #0x03
  str   w10, [x9, #0x0C]            // LCR: 8N1, DLAB off
  mov   w10, #0x06
  str   w10, [x9, #0xA4]            // HALT: + CHANGE_UPDATE
  mov   x11, #0x100000
5:
  ldr   w10, [x9, #0xA4]
  tbz   w10, #2, 6f                 // wait until the update is taken
  subs  x11, x11, #1
  b.ne  5b
6:
  str   wzr, [x9, #0xA4]            // HALT = 0
  mov   w10, #0x07
  str   w10, [x9, #0x08]            // FCR: enable + reset FIFOs
  mov   w10, #0x03
  str   w10, [x9, #0x10]            // MCR: DTR | RTS

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
