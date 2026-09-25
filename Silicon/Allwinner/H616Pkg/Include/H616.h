/** @file
  Allwinner H616 / H618 register map (subset used by this port).

  Values taken from the mainline Linux/U-Boot device tree
  (sun50i-h616.dtsi) and U-Boot's sunxi headers.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef H616_H_
#define H616_H_

//
// Memory map
//
#define H616_DRAM_BASE             0x40000000ULL
#define H616_BL31_BASE             0x40000000ULL   // TF-A BL31 (secmon), no-map
#define H616_BL31_SIZE             0x00040000ULL   // 256 KiB, see sun50i-h616.dtsi
#define H616_PERIPH_BASE           0x01000000ULL
#define H616_PERIPH_SIZE           0x08000000ULL   // 0x0100_0000 .. 0x08FF_FFFF

//
// Clock control unit
//
#define H616_CCU_BASE              0x03001000
#define H616_CCU_PLL_PERIPH0       0x020
#define H616_CCU_SMHC0_CLK         0x830
#define H616_CCU_SMHC_BGR          0x84C          // bus gating (bit n) + reset (bit 16+n)

#define H616_PLL_PERIPH0_N(v)      ((((v) >> 8) & 0xFF) + 1)
#define H616_PLL_PERIPH0_DIV1(v)   (((v) & 0x1) + 1)
#define H616_PLL_PERIPH0_DIV2(v)   ((((v) >> 1) & 0x1) + 1)

#define H616_SMHC_CLK_ENABLE       (1U << 31)
#define H616_SMHC_CLK_SRC_OSC24M   (0U << 24)
#define H616_SMHC_CLK_SRC_PERIPH0  (1U << 24)
#define H616_SMHC_CLK_N(n)         ((n) << 8)     // log2 pre-divider
#define H616_SMHC_CLK_M(m)         ((m) - 1)      // divider 1..16

//
// GPIO (main PIO, banks A..I). Bank stride is 0x24 on H616.
//
#define H616_PIO_BASE              0x0300B000
#define H616_PIO_BANK(n)           (H616_PIO_BASE + (n) * 0x24)
#define H616_PIO_CFG(n, pin)       (H616_PIO_BANK (n) + 0x00 + ((pin) / 8) * 4)
#define H616_PIO_DAT(n)            (H616_PIO_BANK (n) + 0x10)
#define H616_PIO_DRV(n, pin)       (H616_PIO_BANK (n) + 0x14 + ((pin) / 16) * 4)
#define H616_PIO_PULL(n, pin)      (H616_PIO_BANK (n) + 0x1C + ((pin) / 16) * 4)
#define H616_PIO_PORT_F            5
#define H616_PIO_PORT_H            7
#define H616_GPF_SDC0_FUNC         2

//
// Interrupt controller (GIC-400, GICv2)
//
#define H616_GICD_BASE             0x03021000
#define H616_GICC_BASE             0x03022000

//
// Debug UART (UART0 on PH0/PH1, header pins 8/10). DesignWare APB 16550.
//
#define H616_UART0_BASE            0x05000000
#define H616_UART_CLOCK            24000000

//
// SD/MMC host controller registers (SMHC)
//
#define SMHC_GCTRL                 0x000
#define SMHC_CLKCR                 0x004
#define SMHC_TIMEOUT               0x008
#define SMHC_WIDTH                 0x00C
#define SMHC_BLKSZ                 0x010
#define SMHC_BYTECNT               0x014
#define SMHC_CMD                   0x018
#define SMHC_ARG                   0x01C
#define SMHC_RESP0                 0x020
#define SMHC_RESP1                 0x024
#define SMHC_RESP2                 0x028
#define SMHC_RESP3                 0x02C
#define SMHC_IMASK                 0x030
#define SMHC_MINT                  0x034
#define SMHC_RINT                  0x038
#define SMHC_STATUS                0x03C
#define SMHC_FTRGLEVEL             0x040
#define SMHC_NTSR                  0x05C
#define SMHC_HWRST                 0x078
#define SMHC_DMAC                  0x080
#define SMHC_THLDC                 0x100
#define SMHC_SAMP_DL               0x144
#define SMHC_FIFO                  0x200

#define SMHC_GCTRL_SOFT_RESET      (1U << 0)
#define SMHC_GCTRL_FIFO_RESET      (1U << 1)
#define SMHC_GCTRL_DMA_RESET       (1U << 2)
#define SMHC_GCTRL_RESET           (SMHC_GCTRL_SOFT_RESET | SMHC_GCTRL_FIFO_RESET | SMHC_GCTRL_DMA_RESET)
#define SMHC_GCTRL_ACCESS_BY_AHB   (1U << 31)

#define SMHC_CLKCR_ENABLE          (1U << 16)
#define SMHC_CLKCR_DIV_MASK        0xFF

#define SMHC_CMD_RESP_EXPIRE       (1U << 6)
#define SMHC_CMD_LONG_RESPONSE     (1U << 7)
#define SMHC_CMD_CHK_RESPONSE_CRC  (1U << 8)
#define SMHC_CMD_DATA_EXPIRE       (1U << 9)
#define SMHC_CMD_WRITE             (1U << 10)
#define SMHC_CMD_AUTO_STOP         (1U << 12)
#define SMHC_CMD_WAIT_PRE_OVER     (1U << 13)
#define SMHC_CMD_SEND_INIT_SEQ     (1U << 15)
#define SMHC_CMD_UPCLK_ONLY        (1U << 21)
#define SMHC_CMD_START             (1U << 31)

#define SMHC_RINT_RESP_ERROR       (1U << 1)
#define SMHC_RINT_COMMAND_DONE     (1U << 2)
#define SMHC_RINT_DATA_OVER        (1U << 3)
#define SMHC_RINT_RESP_CRC_ERROR   (1U << 6)
#define SMHC_RINT_DATA_CRC_ERROR   (1U << 7)
#define SMHC_RINT_RESP_TIMEOUT     (1U << 8)
#define SMHC_RINT_DATA_TIMEOUT     (1U << 9)
#define SMHC_RINT_VOLT_CHANGE_DONE (1U << 10)
#define SMHC_RINT_FIFO_RUN_ERROR   (1U << 11)
#define SMHC_RINT_HW_LOCKED        (1U << 12)
#define SMHC_RINT_START_BIT_ERROR  (1U << 13)
#define SMHC_RINT_AUTO_CMD_DONE    (1U << 14)
#define SMHC_RINT_END_BIT_ERROR    (1U << 15)
#define SMHC_RINT_ERROR_MASK       0xBFC2

#define SMHC_STATUS_FIFO_EMPTY     (1U << 2)
#define SMHC_STATUS_FIFO_FULL      (1U << 3)
#define SMHC_STATUS_CARD_BUSY      (1U << 9)
#define SMHC_STATUS_FIFO_LEVEL(s)  (((s) >> 17) & 0x3FFF)

#define SMHC_NTSR_MODE_SEL_NEW     (1U << 31)
#define SMHC_SAMP_DL_SW_EN         (1U << 7)
#define SMHC_THLDC_READ_EN         (1U << 0)
#define SMHC_THLDC_WRITE_EN        (1U << 2)
#define SMHC_THLDC_READ_THLD(x)    (((x) & 0xFFF) << 16)

#endif // H616_H_
