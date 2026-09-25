/** @file
  Minimal Synopsys DesignWare HDMI TX driver for EDK2.

  Ported from U-Boot drivers/video/dw_hdmi.c
    Copyright (c) 2015 Google, Inc
    Copyright 2014 Rockchip Inc.
    Copyright (C) 2011-2013 Freescale Semiconductor, Inc.
  Only RGB888 in / RGB888 out, no audio, no HDCP.

  SPDX-License-Identifier: GPL-2.0+
**/

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#include "DwHdmi.h"
#include "DwHdmiRegs.h"

UINT8
DwHdmiRead (
  IN DW_HDMI  *Hdmi,
  IN UINTN    Offset
  )
{
  return MmioRead8 (Hdmi->IoAddr + Offset);
}

STATIC
VOID
HdmiWrite (
  IN DW_HDMI  *Hdmi,
  IN UINT8    Val,
  IN UINTN    Offset
  )
{
  MmioWrite8 (Hdmi->IoAddr + Offset, Val);
}

STATIC
VOID
HdmiMod (
  IN DW_HDMI  *Hdmi,
  IN UINTN    Reg,
  IN UINT8    Mask,
  IN UINT8    Data
  )
{
  UINT8  Val;

  Val  = DwHdmiRead (Hdmi, Reg) & ~Mask;
  Val |= Data & Mask;
  HdmiWrite (Hdmi, Val, Reg);
}

//
// ---------------------------------------------------------------- PHY ---
//

STATIC
VOID
PhyTestClear (
  IN DW_HDMI  *Hdmi,
  IN UINT8    Bit
  )
{
  HdmiMod (Hdmi, HDMI_PHY_TST0, HDMI_PHY_TST0_TSTCLR_MASK, Bit << HDMI_PHY_TST0_TSTCLR_OFFSET);
}

STATIC
EFI_STATUS
PhyWaitI2cDone (
  IN DW_HDMI  *Hdmi,
  IN UINTN    Msec
  )
{
  UINTN  Loops;
  UINT8  Val;

  for (Loops = Msec * 10; Loops > 0; Loops--) {
    Val = DwHdmiRead (Hdmi, HDMI_IH_I2CMPHY_STAT0);
    if ((Val & 0x3) != 0) {
      HdmiWrite (Hdmi, Val, HDMI_IH_I2CMPHY_STAT0);
      return EFI_SUCCESS;
    }

    MicroSecondDelay (100);
  }

  return EFI_TIMEOUT;
}

STATIC
VOID
PhyI2cWrite (
  IN DW_HDMI  *Hdmi,
  IN UINT32   Data,
  IN UINT8    Addr
  )
{
  HdmiWrite (Hdmi, 0xFF, HDMI_IH_I2CMPHY_STAT0);
  HdmiWrite (Hdmi, Addr, HDMI_PHY_I2CM_ADDRESS_ADDR);
  HdmiWrite (Hdmi, (UINT8)(Data >> 8), HDMI_PHY_I2CM_DATAO_1_ADDR);
  HdmiWrite (Hdmi, (UINT8)(Data >> 0), HDMI_PHY_I2CM_DATAO_0_ADDR);
  HdmiWrite (Hdmi, HDMI_PHY_I2CM_OPERATION_ADDR_WRITE, HDMI_PHY_I2CM_OPERATION_ADDR);
  if (EFI_ERROR (PhyWaitI2cDone (Hdmi, 1000))) {
    DEBUG ((DEBUG_WARN, "DwHdmi: PHY I2C write 0x%02x timeout\n", Addr));
  }
}

#define PHY_CONF0_MOD(Hdmi, Mask, Off, En) \
  HdmiMod ((Hdmi), HDMI_PHY_CONF0, (Mask), (UINT8)((En) << (Off)))

STATIC
EFI_STATUS
PhyConfigure (
  IN DW_HDMI  *Hdmi,
  IN UINT32   PixelClock
  )
{
  UINTN  I;
  UINTN  Loops;

  // gen2 tx power off, pddq on
  PHY_CONF0_MOD (Hdmi, HDMI_PHY_CONF0_GEN2_TXPWRON_MASK, HDMI_PHY_CONF0_GEN2_TXPWRON_OFFSET, 0);
  PHY_CONF0_MOD (Hdmi, HDMI_PHY_CONF0_GEN2_PDDQ_MASK, HDMI_PHY_CONF0_GEN2_PDDQ_OFFSET, 1);

  // PHY reset
  HdmiWrite (Hdmi, HDMI_MC_PHYRSTZ_DEASSERT, HDMI_MC_PHYRSTZ);
  HdmiWrite (Hdmi, HDMI_MC_PHYRSTZ_ASSERT, HDMI_MC_PHYRSTZ);
  HdmiWrite (Hdmi, HDMI_MC_HEACPHY_RST_ASSERT, HDMI_MC_HEACPHY_RST);

  PhyTestClear (Hdmi, 1);
  HdmiWrite (Hdmi, HDMI_PHY_I2CM_SLAVE_ADDR_PHY_GEN2, HDMI_PHY_I2CM_SLAVE_ADDR);
  PhyTestClear (Hdmi, 0);

  for (I = 0; Hdmi->MpllCfg[I].PixelClock != MAX_UINT64; I++) {
    if (PixelClock <= Hdmi->MpllCfg[I].PixelClock) {
      break;
    }
  }

  PhyI2cWrite (Hdmi, Hdmi->MpllCfg[I].Cpce, PHY_OPMODE_PLLCFG);
  PhyI2cWrite (Hdmi, Hdmi->MpllCfg[I].Gmp, PHY_PLLGMPCTRL);
  PhyI2cWrite (Hdmi, Hdmi->MpllCfg[I].Curr, PHY_PLLCURRCTRL);
  PhyI2cWrite (Hdmi, 0x0000, PHY_PLLPHBYCTRL);
  PhyI2cWrite (Hdmi, 0x0006, PHY_PLLCLKBISTPHASE);

  for (I = 0; Hdmi->PhyCfg[I].PixelClock != MAX_UINT64; I++) {
    if (PixelClock <= Hdmi->PhyCfg[I].PixelClock) {
      break;
    }
  }

  PhyI2cWrite (Hdmi, Hdmi->PhyCfg[I].Term, PHY_TXTERM);
  PhyI2cWrite (Hdmi, Hdmi->PhyCfg[I].SymCtr, PHY_CKSYMTXCTRL);
  PhyI2cWrite (Hdmi, Hdmi->PhyCfg[I].VlevCtr, PHY_VLEVCTRL);

  // remove clk term
  PhyI2cWrite (Hdmi, 0x8000, PHY_CKCALCTRL);

  PHY_CONF0_MOD (Hdmi, HDMI_PHY_CONF0_PDZ_MASK, HDMI_PHY_CONF0_PDZ_OFFSET, 1);
  PHY_CONF0_MOD (Hdmi, HDMI_PHY_CONF0_ENTMDS_MASK, HDMI_PHY_CONF0_ENTMDS_OFFSET, 0);
  PHY_CONF0_MOD (Hdmi, HDMI_PHY_CONF0_ENTMDS_MASK, HDMI_PHY_CONF0_ENTMDS_OFFSET, 1);
  PHY_CONF0_MOD (Hdmi, HDMI_PHY_CONF0_GEN2_TXPWRON_MASK, HDMI_PHY_CONF0_GEN2_TXPWRON_OFFSET, 1);
  PHY_CONF0_MOD (Hdmi, HDMI_PHY_CONF0_GEN2_PDDQ_MASK, HDMI_PHY_CONF0_GEN2_PDDQ_OFFSET, 0);
  PHY_CONF0_MOD (Hdmi, HDMI_PHY_CONF0_SPARECTRL_MASK, HDMI_PHY_CONF0_SPARECTRL_OFFSET, 1);

  // wait for PHY PLL lock
  for (Loops = 0; Loops < 50; Loops++) {
    if ((DwHdmiRead (Hdmi, HDMI_PHY_STAT0) & HDMI_PHY_TX_PHY_LOCK) != 0) {
      DEBUG ((DEBUG_INFO, "DwHdmi: PHY PLL locked (%u us)\n", (UINT32)Loops * 100));
      return EFI_SUCCESS;
    }

    MicroSecondDelay (100);
  }

  DEBUG ((DEBUG_WARN, "DwHdmi: PHY PLL not locked (STAT0=0x%02x)\n", DwHdmiRead (Hdmi, HDMI_PHY_STAT0)));
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
PhyCfg (
  IN DW_HDMI  *Hdmi,
  IN UINT32   PixelClock
  )
{
  UINTN       I;
  EFI_STATUS  Status;

  Status = EFI_SUCCESS;
  // The HDMI PHY spec says to do the initialisation sequence twice.
  for (I = 0; I < 2; I++) {
    PHY_CONF0_MOD (Hdmi, HDMI_PHY_CONF0_SELDATAENPOL_MASK, HDMI_PHY_CONF0_SELDATAENPOL_OFFSET, 1);
    PHY_CONF0_MOD (Hdmi, HDMI_PHY_CONF0_SELDIPIF_MASK, HDMI_PHY_CONF0_SELDIPIF_OFFSET, 0);
    PHY_CONF0_MOD (Hdmi, HDMI_PHY_CONF0_ENTMDS_MASK, HDMI_PHY_CONF0_ENTMDS_OFFSET, 0);
    PHY_CONF0_MOD (Hdmi, HDMI_PHY_CONF0_PDZ_MASK, HDMI_PHY_CONF0_PDZ_OFFSET, 0);
    Status = PhyConfigure (Hdmi, PixelClock);
  }

  return Status;
}

//
// ------------------------------------------------------------- video ---
//

STATIC
VOID
AvComposer (
  IN DW_HDMI               *Hdmi,
  IN CONST DISPLAY_TIMING  *T
  )
{
  UINT32  Hbl;
  UINT32  Vbl;
  UINT8   Inv;

  Hbl = T->HBackPorch + T->HFrontPorch + T->HSyncLen;
  Vbl = T->VBackPorch + T->VFrontPorch + T->VSyncLen;

  Inv  = HDMI_FC_INVIDCONF_HDCP_KEEPOUT_INACTIVE;
  Inv |= T->VSyncHigh ? HDMI_FC_INVIDCONF_VSYNC_IN_POLARITY_ACTIVE_HIGH :
         HDMI_FC_INVIDCONF_VSYNC_IN_POLARITY_ACTIVE_LOW;
  Inv |= T->HSyncHigh ? HDMI_FC_INVIDCONF_HSYNC_IN_POLARITY_ACTIVE_HIGH :
         HDMI_FC_INVIDCONF_HSYNC_IN_POLARITY_ACTIVE_LOW;
  Inv |= HDMI_FC_INVIDCONF_DE_IN_POLARITY_ACTIVE_HIGH;
  Inv |= T->HdmiMonitor ? HDMI_FC_INVIDCONF_DVI_MODEZ_HDMI_MODE :
         HDMI_FC_INVIDCONF_DVI_MODEZ_DVI_MODE;
  Inv |= HDMI_FC_INVIDCONF_R_V_BLANK_IN_OSC_ACTIVE_LOW;
  Inv |= HDMI_FC_INVIDCONF_IN_I_P_PROGRESSIVE;
  HdmiWrite (Hdmi, Inv, HDMI_FC_INVIDCONF);

  HdmiWrite (Hdmi, (UINT8)(T->HActive >> 8), HDMI_FC_INHACTV1);
  HdmiWrite (Hdmi, (UINT8)T->HActive, HDMI_FC_INHACTV0);
  HdmiWrite (Hdmi, (UINT8)(T->VActive >> 8), HDMI_FC_INVACTV1);
  HdmiWrite (Hdmi, (UINT8)T->VActive, HDMI_FC_INVACTV0);
  HdmiWrite (Hdmi, (UINT8)(Hbl >> 8), HDMI_FC_INHBLANK1);
  HdmiWrite (Hdmi, (UINT8)Hbl, HDMI_FC_INHBLANK0);
  HdmiWrite (Hdmi, (UINT8)Vbl, HDMI_FC_INVBLANK);
  HdmiWrite (Hdmi, (UINT8)(T->HFrontPorch >> 8), HDMI_FC_HSYNCINDELAY1);
  HdmiWrite (Hdmi, (UINT8)T->HFrontPorch, HDMI_FC_HSYNCINDELAY0);
  HdmiWrite (Hdmi, (UINT8)T->VFrontPorch, HDMI_FC_VSYNCINDELAY);
  HdmiWrite (Hdmi, (UINT8)(T->HSyncLen >> 8), HDMI_FC_HSYNCINWIDTH1);
  HdmiWrite (Hdmi, (UINT8)T->HSyncLen, HDMI_FC_HSYNCINWIDTH0);
  HdmiWrite (Hdmi, (UINT8)T->VSyncLen, HDMI_FC_VSYNCINWIDTH);
}

STATIC
VOID
EnableVideoPath (
  IN DW_HDMI  *Hdmi
  )
{
  UINT8  ClkDis;

  HdmiWrite (Hdmi, 12, HDMI_FC_CTRLDUR);
  HdmiWrite (Hdmi, 32, HDMI_FC_EXCTRLDUR);
  HdmiWrite (Hdmi, 1, HDMI_FC_EXCTRLSPAC);

  HdmiWrite (Hdmi, 0x0B, HDMI_FC_CH0PREAM);
  HdmiWrite (Hdmi, 0x16, HDMI_FC_CH1PREAM);
  HdmiWrite (Hdmi, 0x21, HDMI_FC_CH2PREAM);

  HdmiWrite (Hdmi, HDMI_MC_FLOWCTRL_FEED_THROUGH_OFF_CSC_BYPASS, HDMI_MC_FLOWCTRL);

  ClkDis  = 0x7F;
  ClkDis &= ~HDMI_MC_CLKDIS_PIXELCLK_DISABLE;
  HdmiWrite (Hdmi, ClkDis, HDMI_MC_CLKDIS);
  ClkDis &= ~HDMI_MC_CLKDIS_TMDSCLK_DISABLE;
  HdmiWrite (Hdmi, ClkDis, HDMI_MC_CLKDIS);
}

STATIC
VOID
VideoPacketize (
  IN DW_HDMI  *Hdmi
  )
{
  HdmiWrite (Hdmi, 0, HDMI_VP_PR_CD);   // colour depth 24 bpp, no pixel repetition
  HdmiMod (Hdmi, HDMI_VP_STUFF, HDMI_VP_STUFF_PR_STUFFING_MASK, HDMI_VP_STUFF_PR_STUFFING_STUFFING_MODE);
  HdmiMod (
    Hdmi,
    HDMI_VP_CONF,
    HDMI_VP_CONF_PR_EN_MASK | HDMI_VP_CONF_BYPASS_SELECT_MASK,
    HDMI_VP_CONF_PR_EN_DISABLE | HDMI_VP_CONF_BYPASS_SELECT_VID_PACKETIZER
    );
  HdmiMod (Hdmi, HDMI_VP_STUFF, HDMI_VP_STUFF_IDEFAULT_PHASE_MASK, 1 << HDMI_VP_STUFF_IDEFAULT_PHASE_OFFSET);
  HdmiWrite (Hdmi, HDMI_VP_REMAP_YCC422_16BIT, HDMI_VP_REMAP);
  HdmiMod (
    Hdmi,
    HDMI_VP_CONF,
    HDMI_VP_CONF_BYPASS_EN_MASK | HDMI_VP_CONF_PP_EN_ENMASK | HDMI_VP_CONF_YCC422_EN_MASK,
    HDMI_VP_CONF_BYPASS_EN_ENABLE | HDMI_VP_CONF_PP_EN_DISABLE | HDMI_VP_CONF_YCC422_EN_DISABLE
    );
  HdmiMod (
    Hdmi,
    HDMI_VP_STUFF,
    HDMI_VP_STUFF_PP_STUFFING_MASK | HDMI_VP_STUFF_YCC422_STUFFING_MASK,
    HDMI_VP_STUFF_PP_STUFFING_STUFFING_MODE | HDMI_VP_STUFF_YCC422_STUFFING_STUFFING_MODE
    );
  HdmiMod (Hdmi, HDMI_VP_CONF, HDMI_VP_CONF_OUTPUT_SELECTOR_MASK, HDMI_VP_CONF_OUTPUT_SELECTOR_BYPASS);
}

STATIC CONST UINT16  mCscIdentity[3][4] = {
  { 0x2000, 0x0000, 0x0000, 0x0000 },
  { 0x0000, 0x2000, 0x0000, 0x0000 },
  { 0x0000, 0x0000, 0x2000, 0x0000 }
};

STATIC
VOID
VideoCsc (
  IN DW_HDMI  *Hdmi
  )
{
  UINTN  I;

  HdmiWrite (Hdmi, HDMI_CSC_CFG_INTMODE_DISABLE, HDMI_CSC_CFG);
  HdmiMod (Hdmi, HDMI_CSC_SCALE, HDMI_CSC_SCALE_CSC_COLORDE_PTH_MASK, HDMI_CSC_SCALE_CSC_COLORDE_PTH_24BPP);
  for (I = 0; I < 4; I++) {
    HdmiWrite (Hdmi, (UINT8)(mCscIdentity[0][I] & 0xFF), HDMI_CSC_COEF_A1_LSB + I * 2);
    HdmiWrite (Hdmi, (UINT8)(mCscIdentity[0][I] >> 8), HDMI_CSC_COEF_A1_MSB + I * 2);
    HdmiWrite (Hdmi, (UINT8)(mCscIdentity[1][I] & 0xFF), HDMI_CSC_COEF_B1_LSB + I * 2);
    HdmiWrite (Hdmi, (UINT8)(mCscIdentity[1][I] >> 8), HDMI_CSC_COEF_B1_MSB + I * 2);
    HdmiWrite (Hdmi, (UINT8)(mCscIdentity[2][I] & 0xFF), HDMI_CSC_COEF_C1_LSB + I * 2);
    HdmiWrite (Hdmi, (UINT8)(mCscIdentity[2][I] >> 8), HDMI_CSC_COEF_C1_MSB + I * 2);
  }

  HdmiMod (Hdmi, HDMI_CSC_SCALE, HDMI_CSC_SCALE_CSCSCALE_MASK, 1);
}

STATIC
VOID
VideoSample (
  IN DW_HDMI  *Hdmi
  )
{
  UINT8  Val;

  // RGB888 input (video mapping 0x01)
  Val = HDMI_TX_INVID0_INTERNAL_DE_GENERATOR_DISABLE |
        ((0x01 << HDMI_TX_INVID0_VIDEO_MAPPING_OFFSET) & HDMI_TX_INVID0_VIDEO_MAPPING_MASK);
  HdmiWrite (Hdmi, Val, HDMI_TX_INVID0);

  Val = HDMI_TX_INSTUFFING_BDBDATA_STUFFING_ENABLE |
        HDMI_TX_INSTUFFING_RCRDATA_STUFFING_ENABLE |
        HDMI_TX_INSTUFFING_GYDATA_STUFFING_ENABLE;
  HdmiWrite (Hdmi, Val, HDMI_TX_INSTUFFING);
  HdmiWrite (Hdmi, 0, HDMI_TX_GYDATA0);
  HdmiWrite (Hdmi, 0, HDMI_TX_GYDATA1);
  HdmiWrite (Hdmi, 0, HDMI_TX_RCRDATA0);
  HdmiWrite (Hdmi, 0, HDMI_TX_RCRDATA1);
  HdmiWrite (Hdmi, 0, HDMI_TX_BCBDATA0);
  HdmiWrite (Hdmi, 0, HDMI_TX_BCBDATA1);
}

STATIC
VOID
ClearOverflow (
  IN DW_HDMI  *Hdmi
  )
{
  UINT8  Val;
  UINTN  Count;

  HdmiWrite (Hdmi, (UINT8) ~HDMI_MC_SWRSTZ_TMDSSWRST_REQ, HDMI_MC_SWRSTZ);
  Val = DwHdmiRead (Hdmi, HDMI_FC_INVIDCONF);
  for (Count = 0; Count < 4; Count++) {
    HdmiWrite (Hdmi, Val, HDMI_FC_INVIDCONF);
  }
}

//
// ------------------------------------------------------------ public ---
//

VOID
DwHdmiInit (
  IN DW_HDMI  *Hdmi
  )
{
  // mute all interrupts at top level, we poll
  HdmiWrite (Hdmi, HDMI_IH_MUTE_MUTE_WAKEUP_INTERRUPT | HDMI_IH_MUTE_MUTE_ALL_INTERRUPT, HDMI_IH_MUTE);
  HdmiWrite (Hdmi, (UINT8) ~0x04, HDMI_I2CM_INT);
  HdmiWrite (Hdmi, (UINT8) ~0x44, HDMI_I2CM_CTLINT);
}

VOID
DwHdmiPhyInit (
  IN DW_HDMI  *Hdmi
  )
{
  HdmiWrite (Hdmi, HDMI_PHY_I2CM_INT_ADDR_DONE_POL, HDMI_PHY_I2CM_INT_ADDR);
  HdmiWrite (
    Hdmi,
    HDMI_PHY_I2CM_CTLINT_ADDR_NAC_POL | HDMI_PHY_I2CM_CTLINT_ADDR_ARBITRATION_POL,
    HDMI_PHY_I2CM_CTLINT_ADDR
    );
  HdmiWrite (Hdmi, (UINT8) ~HDMI_PHY_HPD, HDMI_PHY_MASK0);
  HdmiWrite (Hdmi, HDMI_IH_PHY_STAT0_HPD, HDMI_IH_PHY_STAT0);
}

BOOLEAN
DwHdmiHotPlugDetected (
  IN DW_HDMI  *Hdmi
  )
{
  return (DwHdmiRead (Hdmi, HDMI_PHY_STAT0) & HDMI_PHY_HPD) != 0;
}

EFI_STATUS
DwHdmiReadEdid (
  IN  DW_HDMI  *Hdmi,
  IN  UINTN    Block,
  OUT UINT8    *Buffer
  )
{
  UINT8  Shift;
  UINTN  Try;
  UINTN  N;
  UINTN  Loops;
  UINT8  Val;
  BOOLEAN Err;

  Shift = (UINT8)((Block % 2) * 0x80);

  HdmiWrite (Hdmi, Hdmi->I2cClkHigh, HDMI_I2CM_SS_SCL_HCNT_0_ADDR);
  HdmiWrite (Hdmi, Hdmi->I2cClkLow, HDMI_I2CM_SS_SCL_LCNT_0_ADDR);
  HdmiMod (Hdmi, HDMI_I2CM_DIV, HDMI_I2CM_DIV_FAST_STD_MODE, HDMI_I2CM_DIV_STD_MODE);
  HdmiWrite (Hdmi, HDMI_I2CM_SLAVE_DDC_ADDR, HDMI_I2CM_SLAVE);
  HdmiWrite (Hdmi, HDMI_I2CM_SEGADDR_DDC, HDMI_I2CM_SEGADDR);
  HdmiWrite (Hdmi, (UINT8)(Block >> 1), HDMI_I2CM_SEGPTR);

  for (Try = 0; Try < 5; Try++) {
    Err = FALSE;
    for (N = 0; N < 128; N++) {
      HdmiWrite (Hdmi, (UINT8)(Shift + N), HDMI_I2CM_ADDRESS);
      HdmiWrite (Hdmi, (Block == 0) ? HDMI_I2CM_OP_RD8 : HDMI_I2CM_OP_RD8_EXT, HDMI_I2CM_OPERATION);

      for (Loops = 0; Loops < 100; Loops++) {
        Val = DwHdmiRead (Hdmi, HDMI_IH_I2CM_STAT0);
        if ((Val & 0x2) != 0) {
          HdmiWrite (Hdmi, Val, HDMI_IH_I2CM_STAT0);
          break;
        }

        MicroSecondDelay (100);
      }

      if (Loops == 100) {
        HdmiMod (Hdmi, HDMI_I2CM_SOFTRSTZ, HDMI_I2CM_SOFTRSTZ_MASK, 0);
        Err = TRUE;
        break;
      }

      Buffer[N] = DwHdmiRead (Hdmi, HDMI_I2CM_DATAI);
    }

    if (!Err) {
      return EFI_SUCCESS;
    }
  }

  return EFI_DEVICE_ERROR;
}

EFI_STATUS
DwHdmiEnable (
  IN DW_HDMI               *Hdmi,
  IN CONST DISPLAY_TIMING  *Timing
  )
{
  EFI_STATUS  Status;

  AvComposer (Hdmi, Timing);
  Status = PhyCfg (Hdmi, Timing->PixelClock);
  EnableVideoPath (Hdmi);
  VideoPacketize (Hdmi);
  VideoCsc (Hdmi);
  VideoSample (Hdmi);
  ClearOverflow (Hdmi);
  return Status;
}
