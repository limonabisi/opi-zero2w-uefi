/** @file
  Minimal Synopsys DesignWare HDMI TX driver (port of U-Boot dw_hdmi.c).

  SPDX-License-Identifier: GPL-2.0+
**/

#ifndef DW_HDMI_H_
#define DW_HDMI_H_

#include <Uefi.h>

typedef struct {
  UINT64    PixelClock;       // Hz; ~0 terminates
  UINT32    Cpce;
  UINT32    Gmp;
  UINT32    Curr;
} DW_HDMI_MPLL_CONFIG;

typedef struct {
  UINT64    PixelClock;       // Hz; ~0 terminates
  UINT32    SymCtr;
  UINT32    Term;
  UINT32    VlevCtr;
} DW_HDMI_PHY_CONFIG;

typedef struct {
  UINT32     PixelClock;      // Hz
  UINT32     HActive;
  UINT32     HFrontPorch;
  UINT32     HSyncLen;
  UINT32     HBackPorch;
  UINT32     VActive;
  UINT32     VFrontPorch;
  UINT32     VSyncLen;
  UINT32     VBackPorch;
  BOOLEAN    HSyncHigh;
  BOOLEAN    VSyncHigh;
  BOOLEAN    HdmiMonitor;     // FALSE -> DVI mode (no infoframes / audio)
} DISPLAY_TIMING;

typedef struct {
  UINTN                        IoAddr;
  CONST DW_HDMI_MPLL_CONFIG    *MpllCfg;
  CONST DW_HDMI_PHY_CONFIG     *PhyCfg;
  UINT8                        I2cClkHigh;
  UINT8                        I2cClkLow;
} DW_HDMI;

VOID
DwHdmiInit (
  IN DW_HDMI  *Hdmi
  );

VOID
DwHdmiPhyInit (
  IN DW_HDMI  *Hdmi
  );

BOOLEAN
DwHdmiHotPlugDetected (
  IN DW_HDMI  *Hdmi
  );

EFI_STATUS
DwHdmiReadEdid (
  IN  DW_HDMI  *Hdmi,
  IN  UINTN    Block,
  OUT UINT8    *Buffer         // 128 bytes
  );

EFI_STATUS
DwHdmiEnable (
  IN DW_HDMI               *Hdmi,
  IN CONST DISPLAY_TIMING  *Timing
  );

UINT8
DwHdmiRead (
  IN DW_HDMI  *Hdmi,
  IN UINTN    Offset
  );

#endif // DW_HDMI_H_
