/** @file
  Allwinner H616/H618 HDMI Graphics Output Protocol driver.

  Pipeline:
    PLL_VIDEO0 (594 MHz) --+--> TCON TV0 dot clock
                           +--> HDMI TMDS clock
    framebuffer -> DE33 mixer0 (UI channel 6 -> blender pipe 0)
                -> TCON TOP (mixer0 -> TCON TV0, HDMI src = TV0)
                -> TCON TV0 (channel 1 timing generator)
                -> Synopsys DW-HDMI TX -> H616 HDMI PHY (Synopsys 3D TX style)

  Register-level sequence derived from Linux drivers/gpu/drm/sun4i
  (sun8i_mixer, sun8i_ui_layer, sun8i_tcon_top, sun4i_tcon, sun8i_dw_hdmi,
  sun8i_hdmi_phy) plus the Armbian H616 DE33/TCON-TV/HDMI-PHY patches, and
  U-Boot's dw_hdmi.c.

  SPDX-License-Identifier: GPL-2.0+
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/EdidDiscovered.h>
#include <Protocol/EdidActive.h>

#include <H616.h>
#include "DwHdmi.h"

//
// Clock control unit
//
#define CCU                    H616_CCU_BASE
#define CCU_PLL_VIDEO0         (CCU + 0x040)
#define CCU_DE_CLK             (CCU + 0x600)
#define CCU_DE_BGR             (CCU + 0x60C)
#define CCU_HDMI_CLK           (CCU + 0xB00)
#define CCU_HDMI_SLOW_CLK      (CCU + 0xB04)
#define CCU_HDMI_CEC_CLK       (CCU + 0xB10)
#define CCU_HDMI_BGR           (CCU + 0xB1C)    // bit0 gate, bit16 HDMI rst, bit17 HDMI_SUB rst
#define CCU_TCON_TOP_BGR       (CCU + 0xB5C)
#define CCU_TCON_TV0_CLK       (CCU + 0xB80)
#define CCU_TCON_TV_BGR        (CCU + 0xB9C)
#define CCU_HDCP_CLK           (CCU + 0xC40)
#define CCU_HDCP_BGR           (CCU + 0xC4C)

#define PLL_EN                 BIT31
#define PLL_LOCK_EN            BIT29
#define PLL_LOCK               BIT28
#define PLL_OUT_EN             BIT27

//
// System control: SRAM C to DE
//
#define SYSCON_SRAM_CTRL1      0x03000004
#define SRAM_C_TO_CPU          BIT24

//
// Display engine 3.3
//
#define DE_BASE                0x01000000
#define DE33_CLK               (DE_BASE + 0x8000)     // gates/resets/dividers + plane mux
#define DE33_MIXER0_TOP        (DE_BASE + 0x8100)
#define DE33_PLANES            (DE_BASE + 0x100000)
#define DE33_MIXER0_DISP       (DE_BASE + 0x280000)
#define DE33_BLD               (DE33_MIXER0_DISP + 0x1000)
#define DE33_UI_CH_NUM         6                      // first UI plane
#define DE33_UI_CH             (DE33_PLANES + DE33_UI_CH_NUM * 0x20000 + 0x1000)
#define MIXER_ROUTE_SLOT       1                      // PORT0 mux 0xa980: slot1 = UI plane 6

#define MIXER_SIZE(w, h)       ((((h) - 1) << 16) | ((w) - 1))

//
// TCON TOP / TCON TV0
//
#define TCON_TOP               0x06510000
#define TCON_TOP_PORT_SEL      (TCON_TOP + 0x1C)
#define TCON_TOP_GATE_SRC      (TCON_TOP + 0x20)
#define TCON_TV0               0x06515000
#define TCON_GCTL              0x000
#define   TCON_GCTL_EN         BIT31
#define   TCON_GCTL_PAD_SEL    BIT1                   // H616: route pad to HDMI
#define   TCON_GCTL_IOMAP_T1   BIT0
#define TCON_GINT0             0x004
#define TCON_GINT1             0x008
#define TCON0_IO_POL           0x088
#define TCON1_CTL              0x090
#define   TCON1_EN             BIT31
#define TCON1_BASIC(n)         (0x094 + 4 * (n))
#define TCON1_IO_TRI           0x0F4

//
// HDMI
//
#define HDMI_BASE              0x06000000
#define HDMI_PHY_BASE          0x06010000
#define HDMI_PHY_REXT_CTRL     (HDMI_PHY_BASE + 0x04)

#define SUNXI_HDMI_DP_GUID \
  { 0x3d4b6c0e, 0x8a41, 0x4f3a, { 0x9b, 0x0e, 0x51, 0x6d, 0x2a, 0x7c, 0x44, 0x19 } }

typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} HDMI_DEVICE_PATH;

STATIC HDMI_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    SUNXI_HDMI_DP_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

//
// H616 HDMI PHY tables (8 bpc), from Armbian "drm: sun4i: Add support for
// H616 HDMI PHY" (sun50i_h616_mpll_cfg / cur_ctr / phy_config).
//
STATIC CONST DW_HDMI_MPLL_CONFIG  mH616Mpll[] = {
  { 27000000,   0x00b3, 0x0003, 0x0012 },
  { 74250000,   0x0072, 0x0003, 0x0013 },
  { 148500000,  0x0051, 0x0003, 0x0019 },
  { 297000000,  0x0040, 0x0003, 0x0019 },
  { 594000000,  0x1a40, 0x0003, 0x0010 },
  { MAX_UINT64, 0x0000, 0x0000, 0x0000 }
};

STATIC CONST DW_HDMI_PHY_CONFIG  mH616Phy[] = {
  { 27000000,   0x8009, 0x0007, 0x02b0 },
  { 74250000,   0x8019, 0x0004, 0x0290 },
  { 148500000,  0x8019, 0x0004, 0x0290 },
  { 297000000,  0x8039, 0x0004, 0x022b },
  { 594000000,  0x8029, 0x0000, 0x008a },
  { MAX_UINT64, 0x0000, 0x0000, 0x0000 }
};

// CEA-861 fallbacks
STATIC CONST DISPLAY_TIMING  mMode720p60 = {
  74250000, 1280, 110, 40, 220, 720, 5, 5, 20, TRUE, TRUE, FALSE
};
STATIC CONST DISPLAY_TIMING  mMode1080p60 = {
  148500000, 1920, 88, 44, 148, 1080, 4, 5, 36, TRUE, TRUE, FALSE
};

typedef struct {
  UINT32    N;          // PLL_VIDEO0 multiplier (VCO = 24 MHz * N)
  UINT32    Div;        // pixel = VCO / 4 / Div
} VIDEO_CLOCKS;

STATIC DW_HDMI                               mHdmi;
STATIC DISPLAY_TIMING                        mTiming;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL          mGop;
STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  mModeInfo;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE     mGopMode;
STATIC FRAME_BUFFER_CONFIGURE                *mBltConfig;
STATIC UINTN                                 mBltConfigSize;
STATIC BOOLEAN                               mFbUncached;
STATIC UINT8                                 mEdid[256];
STATIC UINTN                                 mEdidSize;
STATIC EFI_EDID_DISCOVERED_PROTOCOL          mEdidDiscovered;
STATIC EFI_EDID_ACTIVE_PROTOCOL              mEdidActive;

//
// ------------------------------------------------------------ clocks ---
//

/** Find PLL_VIDEO0 N and a shared divider for the requested pixel clock. */
STATIC
BOOLEAN
PickVideoClocks (
  IN  UINT32        PixelHz,
  OUT VIDEO_CLOCKS  *Clk
  )
{
  UINT32  Div;
  UINT64  N;
  UINT64  Got;
  UINT64  Err;
  UINT64  BestErr;

  BestErr = MAX_UINT64;
  for (Div = 1; Div <= 16; Div++) {
    // pixel = 24 MHz * N / 4 / Div  ->  N = pixel * 4 * Div / 24 MHz
    N = DivU64x32 (MultU64x32 ((UINT64)PixelHz * 4, Div) + 12000000, 24000000);
    if ((N < 12) || (N > 100)) {   // VCO 288 MHz .. 2.4 GHz
      continue;
    }

    Got = DivU64x32 (MultU64x32 (24000000, (UINT32)N), 4 * Div);
    Err = (Got > PixelHz) ? (Got - PixelHz) : (PixelHz - Got);
    if (Err < BestErr) {
      BestErr = Err;
      Clk->N   = (UINT32)N;
      Clk->Div = Div;
    }
  }

  // accept up to 0.5 % error
  return (BestErr != MAX_UINT64) && (BestErr * 200 <= PixelHz);
}

STATIC
EFI_STATUS
SetupClocks (
  IN CONST VIDEO_CLOCKS  *Clk
  )
{
  UINTN  Loops;

  // PLL_VIDEO0: VCO = 24 MHz * N, video0 = VCO / 4
  MmioWrite32 (CCU_PLL_VIDEO0, ((Clk->N - 1) << 8));
  MmioWrite32 (CCU_PLL_VIDEO0, PLL_EN | PLL_LOCK_EN | ((Clk->N - 1) << 8));
  for (Loops = 0; Loops < 10000; Loops++) {
    if ((MmioRead32 (CCU_PLL_VIDEO0) & PLL_LOCK) != 0) {
      break;
    }

    MicroSecondDelay (1);
  }

  MmioOr32 (CCU_PLL_VIDEO0, PLL_OUT_EN);
  DEBUG ((
    DEBUG_INFO,
    "SunxiHdmi: PLL_VIDEO0 N=%u -> %u MHz, div %u (%a)\n",
    Clk->N,
    Clk->N * 6,
    Clk->Div,
    ((MmioRead32 (CCU_PLL_VIDEO0) & PLL_LOCK) != 0) ? "locked" : "NOT locked"
    ));

  // DE: PLL_PERIPH0(2x) 1.2 GHz / 2 = 600 MHz
  MmioWrite32 (CCU_DE_CLK, BIT31 | (1U << 24) | (2 - 1));
  MmioOr32 (CCU_DE_BGR, BIT16);
  MmioOr32 (CCU_DE_BGR, BIT0);

  // TCON TOP + TCON TV0
  MmioOr32 (CCU_TCON_TOP_BGR, BIT16);
  MmioOr32 (CCU_TCON_TOP_BGR, BIT0);
  MmioWrite32 (CCU_TCON_TV0_CLK, BIT31 | (0U << 24) | (0U << 8) | (Clk->Div - 1));
  MmioOr32 (CCU_TCON_TV_BGR, BIT16);
  MmioOr32 (CCU_TCON_TV_BGR, BIT0);

  // HDMI: TMDS clock = pixel clock from PLL_VIDEO0, slow (24 MHz), CEC, HDCP
  MmioWrite32 (CCU_HDMI_CLK, BIT31 | (0U << 24) | (Clk->Div - 1));
  MmioWrite32 (CCU_HDMI_SLOW_CLK, BIT31);
  MmioWrite32 (CCU_HDMI_CEC_CLK, BIT31 | BIT30);
  MmioWrite32 (CCU_HDCP_CLK, BIT31 | (0U << 24) | (2 - 1));
  MmioOr32 (CCU_HDCP_BGR, BIT16);
  MmioOr32 (CCU_HDCP_BGR, BIT0);
  MmioOr32 (CCU_HDMI_BGR, BIT16 | BIT17);
  MmioOr32 (CCU_HDMI_BGR, BIT0);

  MicroSecondDelay (100);
  return EFI_SUCCESS;
}

//
// ----------------------------------------------------- display engine ---
//

STATIC
VOID
SetupDe33 (
  IN UINT32  Width,
  IN UINT32  Height,
  IN UINTN   FbBase
  )
{
  UINT32  Size;
  UINTN   I;

  Size = MIXER_SIZE (Width, Height);

  // SRAM C belongs to the display engine
  MmioAnd32 (SYSCON_SRAM_CTRL1, ~SRAM_C_TO_CPU);

  // DE33 clock block: mixer0 mod/bus gate, de-assert reset, divider 1
  MmioAnd32 (DE33_CLK + 0x0C, ~0xFU);
  MmioOr32 (DE33_CLK + 0x08, BIT0);
  MmioOr32 (DE33_CLK + 0x04, BIT0);
  MmioOr32 (DE33_CLK + 0x00, BIT0);
  // plane -> mixer mux: all planes on mixer0; port0 slots = VI0, UI6, UI7, UI8
  MmioWrite32 (DE33_CLK + 0x24, 0);
  MmioWrite32 (DE33_CLK + 0x28, 0x0000A980);

  // mixer0 top: enable, size, clock
  MmioWrite32 (DE33_MIXER0_TOP + 0x00, BIT0);
  MmioWrite32 (DE33_MIXER0_TOP + 0x08, Size);
  MmioWrite32 (DE33_MIXER0_TOP + 0x0C, 1);

  // blender
  MmioWrite32 (DE33_BLD + 0x88, 0xFF000000);                 // background black
  MmioWrite32 (DE33_BLD + 0x04, 0xFF000000);                 // pipe0 fill colour
  for (I = 0; I < 4; I++) {
    MmioWrite32 (DE33_BLD + 0x90 + 4 * I, 0x03010301);       // blend mode
  }

  MmioWrite32 (DE33_BLD + 0x08, Size);                       // pipe0 input size
  MmioWrite32 (DE33_BLD + 0x0C, 0);                          // pipe0 coord
  MmioWrite32 (DE33_BLD + 0x80, MIXER_ROUTE_SLOT << 0);      // pipe0 <- channel slot 1
  MmioWrite32 (DE33_BLD + 0x8C, Size);                       // output size
  MmioWrite32 (DE33_BLD + 0xFC, 0);                          // progressive

  // UI channel, layer 0: XRGB8888, layer alpha 0xff
  MmioWrite32 (DE33_UI_CH + 0x04, Size);                     // layer size
  MmioWrite32 (DE33_UI_CH + 0x08, 0);                        // layer coord
  MmioWrite32 (DE33_UI_CH + 0x0C, Width * 4);                // pitch
  MmioWrite32 (DE33_UI_CH + 0x10, (UINT32)FbBase);           // top address low
  MmioWrite32 (DE33_UI_CH + 0x80, 0);                        // top address high
  MmioWrite32 (DE33_UI_CH + 0x88, Size);                     // overlay size
  MmioWrite32 (
    DE33_UI_CH + 0x00,
    (0xFFU << 24) | (4U << 8) | (1U << 1) | BIT0             // alpha, XRGB8888, layer alpha, enable
    );

  // enable pipe0 + fill colour
  MmioWrite32 (DE33_BLD + 0x00, BIT8 | BIT0);

  DEBUG ((
    DEBUG_INFO,
    "SunxiHdmi: DE33 mixer0 %ux%u fb=0x%lx (UI ch attr=0x%08x, bld ctl=0x%08x)\n",
    Width,
    Height,
    (UINT64)FbBase,
    MmioRead32 (DE33_UI_CH),
    MmioRead32 (DE33_BLD)
    ));
}

STATIC
VOID
SetupTcon (
  IN CONST DISPLAY_TIMING  *T
  )
{
  UINT32  HTotal;
  UINT32  VTotal;
  UINT32  HBp;
  UINT32  VBp;
  UINT32  Delay;
  UINT32  Val;

  // TCON TOP: mixer0 -> TCON TV0 (index 2), HDMI source = TV0, TV0 clock gate
  MmioAndThenOr32 (TCON_TOP_PORT_SEL, ~(UINT32)0x3, 2);
  Val  = MmioRead32 (TCON_TOP_GATE_SRC);
  Val &= ~(0x3U << 28);
  Val |= (1U << 28) | BIT20;
  MmioWrite32 (TCON_TOP_GATE_SRC, Val);

  // TCON TV0 reset state
  MmioWrite32 (TCON_TV0 + TCON_GCTL, 0);
  MmioWrite32 (TCON_TV0 + TCON_GINT0, 0);
  MmioWrite32 (TCON_TV0 + TCON_GINT1, 0);
  MmioWrite32 (TCON_TV0 + TCON1_IO_TRI, 0xFFFFFFFF);

  HTotal = T->HActive + T->HFrontPorch + T->HSyncLen + T->HBackPorch;
  VTotal = T->VActive + T->VFrontPorch + T->VSyncLen + T->VBackPorch;
  HBp    = T->HSyncLen + T->HBackPorch;          // htotal - hsync_start
  VBp    = T->VSyncLen + T->VBackPorch;          // vtotal - vsync_start

  Delay = VTotal - T->VActive - 2;
  if (Delay > 30) {
    Delay = 30;
  }

  MmioWrite32 (TCON_TV0 + TCON1_CTL, (Delay << 4) & 0x1F0);
  Val = (((T->HActive - 1) & 0xFFF) << 16) | ((T->VActive - 1) & 0xFFF);
  MmioWrite32 (TCON_TV0 + TCON1_BASIC (0), Val);   // input
  MmioWrite32 (TCON_TV0 + TCON1_BASIC (1), Val);   // upscale
  MmioWrite32 (TCON_TV0 + TCON1_BASIC (2), Val);   // output
  MmioWrite32 (TCON_TV0 + TCON1_BASIC (3), (((HTotal - 1) & 0x1FFF) << 16) | ((HBp - 1) & 0xFFF));
  MmioWrite32 (TCON_TV0 + TCON1_BASIC (4), (((VTotal * 2) & 0x1FFF) << 16) | ((VBp - 1) & 0xFFF));
  MmioWrite32 (TCON_TV0 + TCON1_BASIC (5), (((T->HSyncLen - 1) & 0x3FF) << 16) | ((T->VSyncLen - 1) & 0x3FF));

  // polarity lives in the channel-0 register on this TCON
  Val = 0;
  if (T->HSyncHigh) {
    Val |= BIT25;
  }

  if (T->VSyncHigh) {
    Val |= BIT24;
  }

  MmioWrite32 (TCON_TV0 + TCON0_IO_POL, Val);

  MmioWrite32 (TCON_TV0 + TCON_GCTL, TCON_GCTL_PAD_SEL | TCON_GCTL_IOMAP_T1);
  MmioOr32 (TCON_TV0 + TCON_GCTL, TCON_GCTL_EN);
  MmioOr32 (TCON_TV0 + TCON1_CTL, TCON1_EN);

  DEBUG ((
    DEBUG_INFO,
    "SunxiHdmi: TCON TV0 %ux%u total %ux%u GCTL=0x%08x CTL=0x%08x\n",
    T->HActive,
    T->VActive,
    HTotal,
    VTotal,
    MmioRead32 (TCON_TV0 + TCON_GCTL),
    MmioRead32 (TCON_TV0 + TCON1_CTL)
    ));
}

//
// ------------------------------------------------------------- EDID ---
//

STATIC
BOOLEAN
ParseDtd (
  IN  CONST UINT8     *D,
  OUT DISPLAY_TIMING  *T
  )
{
  UINT32  HBlank;
  UINT32  VBlank;

  T->PixelClock = ((UINT32)D[0] | ((UINT32)D[1] << 8)) * 10000;
  if (T->PixelClock == 0) {
    return FALSE;
  }

  T->HActive     = D[2] | ((D[4] & 0xF0) << 4);
  HBlank         = D[3] | ((D[4] & 0x0F) << 8);
  T->VActive     = D[5] | ((D[7] & 0xF0) << 4);
  VBlank         = D[6] | ((D[7] & 0x0F) << 8);
  T->HFrontPorch = D[8] | ((D[11] & 0xC0) << 2);
  T->HSyncLen    = D[9] | ((D[11] & 0x30) << 4);
  T->VFrontPorch = (D[10] >> 4) | ((D[11] & 0x0C) << 2);
  T->VSyncLen    = (D[10] & 0x0F) | ((D[11] & 0x03) << 4);
  if ((HBlank <= T->HFrontPorch + T->HSyncLen) || (VBlank <= T->VFrontPorch + T->VSyncLen)) {
    return FALSE;
  }

  T->HBackPorch  = HBlank - T->HFrontPorch - T->HSyncLen;
  T->VBackPorch  = VBlank - T->VFrontPorch - T->VSyncLen;
  T->HSyncHigh   = (D[17] & BIT1) != 0;
  T->VSyncHigh   = (D[17] & BIT2) != 0;
  T->HdmiMonitor = FALSE;
  return (D[17] & BIT7) == 0;   // progressive only
}

STATIC
VOID
ChooseMode (
  OUT DISPLAY_TIMING  *T
  )
{
  STATIC CONST UINT8  Header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
  DISPLAY_TIMING      Pref;
  VIDEO_CLOCKS        Clk;

  STATIC CONST EFI_GUID  VarGuid = SUNXI_HDMI_DP_GUID;
  UINT8                  Forced;
  UINTN                  Size;

  CopyMem (T, &mMode720p60, sizeof (*T));
  mEdidSize = 0;

  //
  // Manual override (EDID is not readable on the Zero 2W):
  //   Shell> setvar HdmiMode -guid 3d4b6c0e-8a41-4f3a-9b0e-516d2a7c4419 -bs -nv =02
  //   00 = automatic, 01 = 1280x720@60, 02 = 1920x1080@60
  //
  Size   = sizeof (Forced);
  Forced = 0;
  if (!EFI_ERROR (gRT->GetVariable (L"HdmiMode", (EFI_GUID *)&VarGuid, NULL, &Size, &Forced)) && (Forced != 0)) {
    CopyMem (T, (Forced == 2) ? &mMode1080p60 : &mMode720p60, sizeof (*T));
    DEBUG ((DEBUG_INFO, "SunxiHdmi: HdmiMode variable forces %ux%u\n", T->HActive, T->VActive));
    return;
  }

  if (!DwHdmiHotPlugDetected (&mHdmi)) {
    DEBUG ((DEBUG_WARN, "SunxiHdmi: no hot-plug signal, using 1280x720@60\n"));
    return;
  }

  if (EFI_ERROR (DwHdmiReadEdid (&mHdmi, 0, mEdid)) || (CompareMem (mEdid, Header, 8) != 0)) {
    //
    // On the Orange Pi Zero 2W the DDC master never gets a clock edge back
    // (SCL held low / not routed), so EDID is not available. Rerouting
    // DDC to PI0/PI1 was tried: those are plain header pins (NACK).
    // The mode can be chosen with the "HdmiMode" variable instead.
    //
    DEBUG ((DEBUG_WARN, "SunxiHdmi: EDID not available (DDC does not respond)\n"));
    return;
  }

  mEdidSize = 128;
  if ((mEdid[0x7E] != 0) && !EFI_ERROR (DwHdmiReadEdid (&mHdmi, 1, mEdid + 128))) {
    mEdidSize = 256;
  }

  DEBUG ((
    DEBUG_INFO,
    "SunxiHdmi: EDID ok, vendor %c%c%c product 0x%04x\n",
    ((mEdid[8] >> 2) & 0x1F) + 'A' - 1,
    (((mEdid[8] & 0x3) << 3) | (mEdid[9] >> 5)) + 'A' - 1,
    (mEdid[9] & 0x1F) + 'A' - 1,
    mEdid[10] | (mEdid[11] << 8)
    ));

  if (!ParseDtd (&mEdid[0x36], &Pref)) {
    DEBUG ((DEBUG_WARN, "SunxiHdmi: no usable preferred timing, using 720p\n"));
    return;
  }

  DEBUG ((
    DEBUG_INFO,
    "SunxiHdmi: monitor prefers %ux%u @ %u kHz\n",
    Pref.HActive,
    Pref.VActive,
    Pref.PixelClock / 1000
    ));

  if ((Pref.HActive > 1920) || (Pref.VActive > 1200) || (Pref.PixelClock > 165000000)) {
    // 4K/high-refresh monitors: every one of them takes 1080p60
    CopyMem (T, &mMode1080p60, sizeof (*T));
    return;
  }

  if (PickVideoClocks (Pref.PixelClock, &Clk)) {
    CopyMem (T, &Pref, sizeof (*T));
  } else {
    DEBUG ((DEBUG_WARN, "SunxiHdmi: cannot synthesise %u Hz, using 720p\n", Pref.PixelClock));
  }
}

//
// -------------------------------------------------------------- GOP ---
//

STATIC
EFI_STATUS
EFIAPI
GopQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  if ((Info == NULL) || (SizeOfInfo == NULL) || (ModeNumber != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  *Info = AllocateCopyPool (sizeof (mModeInfo), &mModeInfo);
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *SizeOfInfo = sizeof (mModeInfo);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
GopSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN UINT32                        ModeNumber
  )
{
  if (ModeNumber != 0) {
    return EFI_UNSUPPORTED;
  }

  // clear the screen
  ZeroMem ((VOID *)(UINTN)mGopMode.FrameBufferBase, mGopMode.FrameBufferSize);
  if (!mFbUncached) {
    WriteBackDataCacheRange ((VOID *)(UINTN)mGopMode.FrameBufferBase, mGopMode.FrameBufferSize);
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
GopBlt (
  IN     EFI_GRAPHICS_OUTPUT_PROTOCOL       *This,
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL      *BltBuffer OPTIONAL,
  IN     EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation,
  IN     UINTN                              SourceX,
  IN     UINTN                              SourceY,
  IN     UINTN                              DestinationX,
  IN     UINTN                              DestinationY,
  IN     UINTN                              Width,
  IN     UINTN                              Height,
  IN     UINTN                              Delta OPTIONAL
  )
{
  EFI_STATUS  Status;
  EFI_TPL     OldTpl;
  UINTN       Line;
  UINTN       Stride;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = FrameBufferBlt (
             mBltConfig,
             BltBuffer,
             BltOperation,
             SourceX,
             SourceY,
             DestinationX,
             DestinationY,
             Width,
             Height,
             Delta
             );

  if (!EFI_ERROR (Status) && !mFbUncached && (BltOperation != EfiBltVideoToBltBuffer)) {
    Stride = mModeInfo.PixelsPerScanLine * 4;
    for (Line = DestinationY; (Line < DestinationY + Height) && (Line < mModeInfo.VerticalResolution); Line++) {
      WriteBackDataCacheRange (
        (VOID *)(UINTN)(mGopMode.FrameBufferBase + Line * Stride + DestinationX * 4),
        Width * 4
        );
    }
  }

  gBS->RestoreTPL (OldTpl);
  return Status;
}

//
// ------------------------------------------------------------- entry ---
//

EFI_STATUS
EFIAPI
SunxiHdmiDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS    Status;
  VIDEO_CLOCKS  Clk;
  UINTN         FbSize;
  UINTN         FbPages;
  VOID          *Fb;
  EFI_HANDLE    Handle;

  mHdmi.IoAddr     = HDMI_BASE;
  mHdmi.MpllCfg    = mH616Mpll;
  mHdmi.PhyCfg     = mH616Phy;
  mHdmi.I2cClkHigh = 96;     // 4.0 us @ 24 MHz isfr: 100 kHz standard mode
  mHdmi.I2cClkLow  = 113;    // 4.7 us

  //
  // Bring up the HDMI block with a safe clock first so HPD / DDC work,
  // then pick the mode and program everything for real.
  //
  CopyMem (&mTiming, &mMode720p60, sizeof (mTiming));
  PickVideoClocks (mTiming.PixelClock, &Clk);
  SetupClocks (&Clk);

  // H6-style PHY wrapper: external resistor calibration
  MmioOr32 (HDMI_PHY_REXT_CTRL, BIT31);
  MmioAndThenOr32 (HDMI_PHY_REXT_CTRL, 0x0000FFFF, 0x80C00000);

  DEBUG ((
    DEBUG_INFO,
    "SunxiHdmi: DW-HDMI design 0x%02x rev 0x%02x product 0x%02x/0x%02x phy type 0x%02x\n",
    DwHdmiRead (&mHdmi, 0x0000),
    DwHdmiRead (&mHdmi, 0x0001),
    DwHdmiRead (&mHdmi, 0x0002),
    DwHdmiRead (&mHdmi, 0x0003),
    DwHdmiRead (&mHdmi, 0x0006)
    ));

  DwHdmiInit (&mHdmi);
  DwHdmiPhyInit (&mHdmi);
  MicroSecondDelay (200000);    // let a freshly plugged monitor settle its DDC

  ChooseMode (&mTiming);
  if (!PickVideoClocks (mTiming.PixelClock, &Clk)) {
    CopyMem (&mTiming, &mMode720p60, sizeof (mTiming));
    PickVideoClocks (mTiming.PixelClock, &Clk);
  }

  SetupClocks (&Clk);

  //
  // Framebuffer: reserved (the OS may keep scanning it out as efifb /
  // simplefb) and mapped write-combining so the DE always sees our writes.
  //
  FbSize  = mTiming.HActive * mTiming.VActive * 4;
  FbPages = EFI_SIZE_TO_PAGES (FbSize);
  Fb      = AllocateReservedPages (FbPages);
  if (Fb == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gDS->SetMemorySpaceAttributes ((EFI_PHYSICAL_ADDRESS)(UINTN)Fb, EFI_PAGES_TO_SIZE (FbPages), EFI_MEMORY_WC);
  mFbUncached = !EFI_ERROR (Status);
  DEBUG ((DEBUG_INFO, "SunxiHdmi: framebuffer @ %p (%u KiB), write-combining: %r\n", Fb, (UINT32)(FbSize / 1024), Status));
  ZeroMem (Fb, FbSize);
  WriteBackInvalidateDataCacheRange (Fb, FbSize);

  SetupDe33 (mTiming.HActive, mTiming.VActive, (UINTN)Fb);
  SetupTcon (&mTiming);

  Status = DwHdmiEnable (&mHdmi, &mTiming);
  DEBUG ((
    DEBUG_INFO,
    "SunxiHdmi: HDMI %ux%u @ %u kHz %a: %r (HPD %a)\n",
    mTiming.HActive,
    mTiming.VActive,
    mTiming.PixelClock / 1000,
    mTiming.HdmiMonitor ? "HDMI" : "DVI",
    Status,
    DwHdmiHotPlugDetected (&mHdmi) ? "yes" : "no"
    ));

  //
  // Graphics Output Protocol
  //
  mModeInfo.Version              = 0;
  mModeInfo.HorizontalResolution = mTiming.HActive;
  mModeInfo.VerticalResolution   = mTiming.VActive;
  mModeInfo.PixelFormat          = PixelBlueGreenRedReserved8BitPerColor;
  mModeInfo.PixelsPerScanLine    = mTiming.HActive;

  mGopMode.MaxMode         = 1;
  mGopMode.Mode            = 0;
  mGopMode.Info            = &mModeInfo;
  mGopMode.SizeOfInfo      = sizeof (mModeInfo);
  mGopMode.FrameBufferBase = (EFI_PHYSICAL_ADDRESS)(UINTN)Fb;
  mGopMode.FrameBufferSize = FbSize;

  mGop.QueryMode = GopQueryMode;
  mGop.SetMode   = GopSetMode;
  mGop.Blt       = GopBlt;
  mGop.Mode      = &mGopMode;

  mBltConfigSize = 0;
  Status         = FrameBufferBltConfigure (Fb, &mModeInfo, mBltConfig, &mBltConfigSize);
  if (Status == RETURN_BUFFER_TOO_SMALL) {
    mBltConfig = AllocatePool (mBltConfigSize);
    Status     = FrameBufferBltConfigure (Fb, &mModeInfo, mBltConfig, &mBltConfigSize);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  mEdidDiscovered.SizeOfEdid = (UINT32)mEdidSize;
  mEdidDiscovered.Edid       = (mEdidSize != 0) ? mEdid : NULL;
  mEdidActive.SizeOfEdid     = mEdidDiscovered.SizeOfEdid;
  mEdidActive.Edid           = mEdidDiscovered.Edid;

  Handle = NULL;
  return gBS->InstallMultipleProtocolInterfaces (
                &Handle,
                &gEfiGraphicsOutputProtocolGuid,
                &mGop,
                &gEfiDevicePathProtocolGuid,
                &mDevicePath,
                &gEfiEdidDiscoveredProtocolGuid,
                &mEdidDiscovered,
                &gEfiEdidActiveProtocolGuid,
                &mEdidActive,
                NULL
                );
}
