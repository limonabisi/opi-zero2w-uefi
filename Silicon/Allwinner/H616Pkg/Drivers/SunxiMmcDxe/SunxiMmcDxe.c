/** @file
  Allwinner H616/H618 SD/MMC host controller (SMHC) driver.

  Produces EMBEDDED_MMC_HOST_PROTOCOL so that EmbeddedPkg/MmcDxe can
  expose the microSD card as a BlockIo device.

  - PIO (CPU) FIFO transfers, no DMA.
  - Clock from OSC24M (<= 24 MHz) or PLL_PERIPH0 (> 24 MHz), same maths
    as U-Boot's drivers/mmc/sunxi_mmc.c.
  - Data commands are deferred: MmcDxe issues SendCommand() before it
    tells us the transfer length, but the SMHC needs BYTECNT programmed
    before the command starts. So data commands are latched in
    SendCommand() and actually started in Read/WriteBlockData().
  - Multi-block transfers use the controller's AUTO_STOP (CMD12 is sent
    by hardware), so the CMD12 MmcDxe sends afterwards is a no-op.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/MmcHost.h>

#include <H616.h>

#define SMHC_REG(Off)  (mBase + (Off))

#define SUNXI_MMC_DEVICE_PATH_GUID \
  { 0xf8c1689c, 0xf461, 0x47e6, { 0xaa, 0x56, 0xbd, 0x39, 0xe5, 0xad, 0xcc, 0x0c } }

typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} SUNXI_MMC_DEVICE_PATH;

STATIC UINTN    mBase;
STATIC UINT32   mIndex;
STATIC UINT32   mCurrentClock;

STATIC BOOLEAN  mLastWasAppCmd;
STATIC BOOLEAN  mPending;
STATIC UINT32   mPendingIdx;
STATIC UINT32   mPendingCmdFlags;
STATIC UINT32   mPendingArg;
STATIC BOOLEAN  mPendingIsWrite;
STATIC BOOLEAN  mPendingMulti;
STATIC UINT32   mResponse[4];

STATIC SUNXI_MMC_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    SUNXI_MMC_DEVICE_PATH_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

//
// ---------------------------------------------------------------------------
// Low level helpers
// ---------------------------------------------------------------------------
//

/** Poll until (Reg & Mask) == Value or timeout (in microseconds). */
STATIC
EFI_STATUS
WaitReg (
  IN UINTN   Reg,
  IN UINT32  Mask,
  IN UINT32  Value,
  IN UINTN   TimeoutUs
  )
{
  while ((MmioRead32 (Reg) & Mask) != Value) {
    if (TimeoutUs-- == 0) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }

  return EFI_SUCCESS;
}

/** Tell the controller to latch the new card-clock settings. */
STATIC
EFI_STATUS
UpdateClock (
  VOID
  )
{
  EFI_STATUS  Status;

  MmioWrite32 (
    SMHC_REG (SMHC_CMD),
    SMHC_CMD_START | SMHC_CMD_UPCLK_ONLY | SMHC_CMD_WAIT_PRE_OVER
    );
  Status = WaitReg (SMHC_REG (SMHC_CMD), SMHC_CMD_START, 0, 2000000);
  // clock update sets various irq status bits, clear them
  MmioWrite32 (SMHC_REG (SMHC_RINT), MmioRead32 (SMHC_REG (SMHC_RINT)));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SunxiMmc: clock update timeout\n"));
  }

  return Status;
}

STATIC
UINT32
GetPllPeriph0Hz (
  VOID
  )
{
  UINT32  Val;

  Val = MmioRead32 (H616_CCU_BASE + H616_CCU_PLL_PERIPH0);
  //
  // The register describes the 2x clock; the SMHC mod clock applies a
  // fixed /2 post divider, so treat it as the 1x rate (see U-Boot).
  //
  return (UINT32)((24000000ULL * H616_PLL_PERIPH0_N (Val)) / 2 /
                  H616_PLL_PERIPH0_DIV1 (Val) / H616_PLL_PERIPH0_DIV2 (Val));
}

STATIC
EFI_STATUS
SetModClock (
  IN UINT32  Hz
  )
{
  UINT32  Src;
  UINT32  ParentHz;
  UINT32  Div;
  UINT32  N;

  if (Hz <= 24000000) {
    Src      = H616_SMHC_CLK_SRC_OSC24M;
    ParentHz = 24000000;
  } else {
    Src      = H616_SMHC_CLK_SRC_PERIPH0;
    ParentHz = GetPllPeriph0Hz ();
  }

  Div = ParentHz / Hz;
  if ((ParentHz % Hz) != 0) {
    Div++;
  }

  N = 0;
  while (Div > 16) {
    N++;
    Div = (Div + 1) / 2;
  }

  if (N > 3) {
    DEBUG ((DEBUG_ERROR, "SunxiMmc: cannot set clock to %u Hz\n", Hz));
    return EFI_UNSUPPORTED;
  }

  // H616 uses the "new timing mode"
  MmioOr32 (SMHC_REG (SMHC_NTSR), SMHC_NTSR_MODE_SEL_NEW);

  MmioWrite32 (
    H616_CCU_BASE + H616_CCU_SMHC0_CLK + mIndex * 4,
    H616_SMHC_CLK_ENABLE | Src | H616_SMHC_CLK_N (N) | H616_SMHC_CLK_M (Div)
    );

  DEBUG ((
    DEBUG_INFO,
    "SunxiMmc: clock req %u Hz parent %u Hz n=%u m=%u -> %u Hz\n",
    Hz,
    ParentHz,
    1U << N,
    Div,
    ParentHz / (1U << N) / Div
    ));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SetCardClock (
  IN UINT32  Hz
  )
{
  EFI_STATUS  Status;
  UINT32      ClkCr;

  ClkCr  = MmioRead32 (SMHC_REG (SMHC_CLKCR));
  ClkCr &= ~SMHC_CLKCR_ENABLE;
  MmioWrite32 (SMHC_REG (SMHC_CLKCR), ClkCr);
  Status = UpdateClock ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SetModClock (Hz);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ClkCr &= ~SMHC_CLKCR_DIV_MASK;
  MmioWrite32 (SMHC_REG (SMHC_CLKCR), ClkCr);

  // Delay of zero before auto calibration (as U-Boot / Linux do)
  MmioWrite32 (SMHC_REG (SMHC_SAMP_DL), SMHC_SAMP_DL_SW_EN);

  ClkCr |= SMHC_CLKCR_ENABLE;
  MmioWrite32 (SMHC_REG (SMHC_CLKCR), ClkCr);
  Status = UpdateClock ();
  if (!EFI_ERROR (Status)) {
    mCurrentClock = Hz;
  }

  return Status;
}

/** Clock gate, reset and pin mux for SMHC0 (PF0..PF5). */
STATIC
VOID
PlatformInit (
  VOID
  )
{
  UINT32  Pin;
  UINT32  Shift;

  // Bus clock gate + de-assert reset
  MmioOr32 (H616_CCU_BASE + H616_CCU_SMHC_BGR, (1U << mIndex) | (1U << (16 + mIndex)));

  if (mIndex == 0) {
    for (Pin = 0; Pin <= 5; Pin++) {
      Shift = (Pin % 8) * 4;
      MmioAndThenOr32 (H616_PIO_CFG (H616_PIO_PORT_F, Pin), ~(0xFU << Shift), H616_GPF_SDC0_FUNC << Shift);
      Shift = (Pin % 16) * 2;
      MmioAndThenOr32 (H616_PIO_PULL (H616_PIO_PORT_F, Pin), ~(0x3U << Shift), 1U << Shift);   // pull-up
      MmioAndThenOr32 (H616_PIO_DRV (H616_PIO_PORT_F, Pin), ~(0x3U << Shift), 2U << Shift);    // drive 2
    }
  }
}

STATIC
VOID
ControllerReset (
  VOID
  )
{
  MmioWrite32 (SMHC_REG (SMHC_GCTRL), SMHC_GCTRL_RESET);
  MicroSecondDelay (1000);

  // Reset card
  MmioWrite32 (SMHC_REG (SMHC_HWRST), 0);
  MicroSecondDelay (10);
  MmioWrite32 (SMHC_REG (SMHC_HWRST), 1);
  MicroSecondDelay (300);

  // FIFO R/W threshold, needed on H616
  MmioWrite32 (
    SMHC_REG (SMHC_THLDC),
    SMHC_THLDC_READ_THLD (512) | SMHC_THLDC_WRITE_EN | SMHC_THLDC_READ_EN
    );

  MmioWrite32 (SMHC_REG (SMHC_TIMEOUT), 0xFFFFFFFF);
  MmioWrite32 (SMHC_REG (SMHC_RINT), 0xFFFFFFFF);
}

/** Recover the controller after an error (same as U-Boot). */
STATIC
VOID
ErrorRecovery (
  VOID
  )
{
  MmioWrite32 (SMHC_REG (SMHC_GCTRL), SMHC_GCTRL_RESET);
  UpdateClock ();
}

STATIC
VOID
FinishCommand (
  VOID
  )
{
  MmioWrite32 (SMHC_REG (SMHC_RINT), 0xFFFFFFFF);
  MmioOr32 (SMHC_REG (SMHC_GCTRL), SMHC_GCTRL_FIFO_RESET);
}

/** Wait for a RINT done bit, bail out on any error bit. */
STATIC
EFI_STATUS
WaitRint (
  IN UINT32  DoneBit,
  IN UINTN   TimeoutUs
  )
{
  UINT32  Rint;

  for ( ; ;) {
    Rint = MmioRead32 (SMHC_REG (SMHC_RINT));
    if ((Rint & SMHC_RINT_ERROR_MASK) != 0) {
      DEBUG ((DEBUG_BLKIO, "SunxiMmc: RINT error 0x%x\n", Rint));
      return ((Rint & SMHC_RINT_RESP_TIMEOUT) != 0) ? EFI_TIMEOUT : EFI_DEVICE_ERROR;
    }

    if ((Rint & DoneBit) != 0) {
      return EFI_SUCCESS;
    }

    if (TimeoutUs-- == 0) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }
}

STATIC
UINT32
BuildCmdValue (
  IN UINT32  Idx,
  IN UINT32  Flags
  )
{
  UINT32  CmdVal;

  CmdVal = SMHC_CMD_START | Idx;
  if (Idx == 0) {
    CmdVal |= SMHC_CMD_SEND_INIT_SEQ;
  }

  if ((Flags & MMC_CMD_WAIT_RESPONSE) != 0) {
    CmdVal |= SMHC_CMD_RESP_EXPIRE;
    if ((Flags & MMC_CMD_LONG_RESPONSE) != 0) {
      CmdVal |= SMHC_CMD_LONG_RESPONSE;
    }

    if ((Flags & MMC_CMD_NO_CRC_RESPONSE) == 0) {
      CmdVal |= SMHC_CMD_CHK_RESPONSE_CRC;
    }
  }

  return CmdVal;
}

STATIC
VOID
LatchResponse (
  VOID
  )
{
  mResponse[0] = MmioRead32 (SMHC_REG (SMHC_RESP0));
  mResponse[1] = MmioRead32 (SMHC_REG (SMHC_RESP1));
  mResponse[2] = MmioRead32 (SMHC_REG (SMHC_RESP2));
  mResponse[3] = MmioRead32 (SMHC_REG (SMHC_RESP3));
}

STATIC
EFI_STATUS
WaitNotBusy (
  VOID
  )
{
  return WaitReg (SMHC_REG (SMHC_STATUS), SMHC_STATUS_CARD_BUSY, 0, 2000000);
}

/** Does this command move data on the DAT lines? (SD card semantics) */
STATIC
BOOLEAN
IsDataCommand (
  IN  UINT32   Idx,
  IN  UINT32   Arg,
  OUT BOOLEAN  *IsWrite
  )
{
  *IsWrite = FALSE;
  switch (Idx) {
    case 17:
    case 18:
      return TRUE;
    case 24:
    case 25:
      *IsWrite = TRUE;
      return TRUE;
    case 51:  // ACMD51 SEND_SCR, 8 bytes
      return mLastWasAppCmd;
    case 13:  // ACMD13 SD_STATUS, 64 bytes
      return mLastWasAppCmd;
    case 6:   // CMD6 SWITCH_FUNC (64 bytes); ACMD6 SET_BUS_WIDTH has no data
      return !mLastWasAppCmd;
    case 8:   // eMMC SEND_EXT_CSD uses arg 0; SD SEND_IF_COND has no data
      return (Arg == 0);
    default:
      return FALSE;
  }
}

//
// ---------------------------------------------------------------------------
// EFI_MMC_HOST_PROTOCOL
// ---------------------------------------------------------------------------
//

STATIC
BOOLEAN
EFIAPI
SunxiMmcIsCardPresent (
  IN EFI_MMC_HOST_PROTOCOL  *This
  )
{
  // We booted from this card, so it is there. (PF6 CD pin not used.)
  return TRUE;
}

STATIC
BOOLEAN
EFIAPI
SunxiMmcIsReadOnly (
  IN EFI_MMC_HOST_PROTOCOL  *This
  )
{
  return FALSE;
}

STATIC
EFI_STATUS
EFIAPI
SunxiMmcBuildDevicePath (
  IN  EFI_MMC_HOST_PROTOCOL     *This,
  OUT EFI_DEVICE_PATH_PROTOCOL  **DevicePath
  )
{
  *DevicePath = DuplicateDevicePath ((EFI_DEVICE_PATH_PROTOCOL *)&mDevicePath);
  return (*DevicePath != NULL) ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

STATIC
EFI_STATUS
EFIAPI
SunxiMmcNotifyState (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN MMC_STATE              State
  )
{
  if (State == MmcHwInitializationState) {
    DEBUG ((DEBUG_INFO, "SunxiMmc: controller init @ 0x%lx\n", (UINT64)mBase));
    mPending       = FALSE;
    mLastWasAppCmd = FALSE;
    PlatformInit ();
    ControllerReset ();
    MmioWrite32 (SMHC_REG (SMHC_WIDTH), 0);
    return SetCardClock (400000);
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SunxiMmcSendCommand (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN MMC_CMD                MmcCmd,
  IN UINT32                 Argument
  )
{
  EFI_STATUS  Status;
  UINT32      Idx;
  BOOLEAN     IsWrite;

  Idx = MMC_GET_INDX (MmcCmd);

  if (Idx == 12) {
    // Multi-block transfers use the hardware AUTO_STOP, nothing to do.
    mLastWasAppCmd = FALSE;
    ZeroMem (mResponse, sizeof (mResponse));
    return EFI_SUCCESS;
  }

  if (IsDataCommand (Idx, Argument, &IsWrite)) {
    // Defer until Read/WriteBlockData() tells us the length.
    mPending         = TRUE;
    mPendingIdx      = Idx;
    mPendingCmdFlags = MmcCmd;
    mPendingArg      = Argument;
    mPendingIsWrite  = IsWrite;
    mPendingMulti    = (Idx == 18) || (Idx == 25);
    mLastWasAppCmd   = FALSE;
    return EFI_SUCCESS;
  }

  mLastWasAppCmd = (Idx == 55);

  MmioWrite32 (SMHC_REG (SMHC_ARG), Argument);
  MmioWrite32 (SMHC_REG (SMHC_CMD), BuildCmdValue (Idx, MmcCmd));

  Status = WaitRint (SMHC_RINT_COMMAND_DONE, 1000000);
  if (!EFI_ERROR (Status) && (Idx == 7)) {
    // R1b: wait for the card to release DAT0
    Status = WaitNotBusy ();
  }

  if (EFI_ERROR (Status)) {
    if (Status != EFI_TIMEOUT) {
      DEBUG ((DEBUG_ERROR, "SunxiMmc: CMD%u arg 0x%x failed: %r\n", Idx, Argument, Status));
    }

    ErrorRecovery ();
    FinishCommand ();
    return Status;
  }

  LatchResponse ();
  FinishCommand ();
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SunxiMmcReceiveResponse (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN MMC_RESPONSE_TYPE      Type,
  IN UINT32                 *Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (Type == MMC_RESPONSE_TYPE_R2) {
    CopyMem (Buffer, mResponse, sizeof (mResponse));
  } else {
    Buffer[0] = mResponse[0];
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
DoDataTransfer (
  IN     UINTN   Length,
  IN OUT UINT8   *Buffer
  )
{
  EFI_STATUS  Status;
  UINT32      CmdVal;
  UINT32      BlockSize;
  UINTN       Words;
  UINTN       Index;
  UINT32      StatusReg;
  UINT32      InFifo;
  UINTN       Spins;
  UINTN       SpinLimit;
  BOOLEAN     Reading;

  if (!mPending) {
    DEBUG ((DEBUG_ERROR, "SunxiMmc: data phase without a data command\n"));
    return EFI_NOT_READY;
  }

  mPending = FALSE;
  Reading  = !mPendingIsWrite;

  if ((Length == 0) || ((Length & 3) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  BlockSize = (Length < 512) ? (UINT32)Length : 512;
  if ((Length % BlockSize) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  CmdVal = BuildCmdValue (mPendingIdx, mPendingCmdFlags) |
           SMHC_CMD_DATA_EXPIRE | SMHC_CMD_WAIT_PRE_OVER;
  if (!Reading) {
    CmdVal |= SMHC_CMD_WRITE;
  }

  if (mPendingMulti) {
    CmdVal |= SMHC_CMD_AUTO_STOP;
  }

  MmioOr32 (SMHC_REG (SMHC_GCTRL), SMHC_GCTRL_ACCESS_BY_AHB);
  MmioWrite32 (SMHC_REG (SMHC_BLKSZ), BlockSize);
  MmioWrite32 (SMHC_REG (SMHC_BYTECNT), (UINT32)Length);
  MmioWrite32 (SMHC_REG (SMHC_ARG), mPendingArg);
  MmioWrite32 (SMHC_REG (SMHC_CMD), CmdVal);

  Words     = Length / 4;
  SpinLimit = 1000000 + Words * 4;   // ~1 s + slack, in 1 us steps
  Spins     = 0;
  Status    = EFI_SUCCESS;

  for (Index = 0; Index < Words; ) {
    StatusReg = MmioRead32 (SMHC_REG (SMHC_STATUS));
    if ((MmioRead32 (SMHC_REG (SMHC_RINT)) & SMHC_RINT_ERROR_MASK) != 0) {
      Status = EFI_DEVICE_ERROR;
      break;
    }

    if ((StatusReg & (Reading ? SMHC_STATUS_FIFO_EMPTY : SMHC_STATUS_FIFO_FULL)) != 0) {
      if (Spins++ > SpinLimit) {
        Status = EFI_TIMEOUT;
        break;
      }

      MicroSecondDelay (1);
      continue;
    }

    if (Reading) {
      InFifo = SMHC_STATUS_FIFO_LEVEL (StatusReg);
      if ((InFifo == 0) && ((StatusReg & SMHC_STATUS_FIFO_FULL) != 0)) {
        InFifo = 32;
      }

      if (InFifo == 0) {
        InFifo = 1;
      }

      for ( ; (InFifo > 0) && (Index < Words); InFifo--, Index++) {
        WriteUnaligned32 ((UINT32 *)(Buffer + Index * 4), MmioRead32 (SMHC_REG (SMHC_FIFO)));
      }
    } else {
      MmioWrite32 (SMHC_REG (SMHC_FIFO), ReadUnaligned32 ((UINT32 *)(Buffer + Index * 4)));
      Index++;
    }
  }

  if (!EFI_ERROR (Status)) {
    Status = WaitRint (SMHC_RINT_COMMAND_DONE, 1000000);
  }

  if (!EFI_ERROR (Status)) {
    Status = WaitRint (mPendingMulti ? SMHC_RINT_AUTO_CMD_DONE : SMHC_RINT_DATA_OVER, 2000000);
  }

  if (!EFI_ERROR (Status) && !Reading) {
    Status = WaitNotBusy ();
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "SunxiMmc: CMD%u arg 0x%x len %u failed: %r (RINT 0x%x)\n",
      mPendingIdx,
      mPendingArg,
      (UINT32)Length,
      Status,
      MmioRead32 (SMHC_REG (SMHC_RINT))
      ));
    ErrorRecovery ();
    FinishCommand ();
    return Status;
  }

  LatchResponse ();
  FinishCommand ();
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SunxiMmcReadBlockData (
  IN  EFI_MMC_HOST_PROTOCOL  *This,
  IN  EFI_LBA                Lba,
  IN  UINTN                  Length,
  OUT UINT32                 *Buffer
  )
{
  if ((Buffer == NULL) || (mPending && mPendingIsWrite)) {
    return EFI_INVALID_PARAMETER;
  }

  return DoDataTransfer (Length, (UINT8 *)Buffer);
}

STATIC
EFI_STATUS
EFIAPI
SunxiMmcWriteBlockData (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN EFI_LBA                Lba,
  IN UINTN                  Length,
  IN UINT32                 *Buffer
  )
{
  if ((Buffer == NULL) || (mPending && !mPendingIsWrite)) {
    return EFI_INVALID_PARAMETER;
  }

  return DoDataTransfer (Length, (UINT8 *)Buffer);
}

STATIC
EFI_STATUS
EFIAPI
SunxiMmcSetIos (
  IN EFI_MMC_HOST_PROTOCOL  *This,
  IN UINT32                 BusClockFreq,
  IN UINT32                 BusWidth,
  IN UINT32                 TimingMode
  )
{
  EFI_STATUS  Status;
  UINT32      MaxClock;

  if (BusClockFreq != 0) {
    MaxClock = FixedPcdGet32 (PcdSunxiMmcMaxClock);
    if (BusClockFreq > MaxClock) {
      BusClockFreq = MaxClock;
    }

    if (BusClockFreq != mCurrentClock) {
      Status = SetCardClock (BusClockFreq);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  switch (BusWidth) {
    case 8:
      MmioWrite32 (SMHC_REG (SMHC_WIDTH), 2);
      break;
    case 4:
      MmioWrite32 (SMHC_REG (SMHC_WIDTH), 1);
      break;
    default:
      MmioWrite32 (SMHC_REG (SMHC_WIDTH), 0);
      break;
  }

  DEBUG ((DEBUG_INFO, "SunxiMmc: SetIos %u Hz, %u-bit\n", mCurrentClock, BusWidth));
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
EFIAPI
SunxiMmcIsMultiBlock (
  IN EFI_MMC_HOST_PROTOCOL  *This
  )
{
  return TRUE;
}

STATIC EFI_MMC_HOST_PROTOCOL  mMmcHost = {
  MMC_HOST_PROTOCOL_REVISION,
  SunxiMmcIsCardPresent,
  SunxiMmcIsReadOnly,
  SunxiMmcBuildDevicePath,
  SunxiMmcNotifyState,
  SunxiMmcSendCommand,
  SunxiMmcReceiveResponse,
  SunxiMmcReadBlockData,
  SunxiMmcWriteBlockData,
  SunxiMmcSetIos,
  SunxiMmcIsMultiBlock
};

EFI_STATUS
EFIAPI
SunxiMmcDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE  Handle;

  mBase  = (UINTN)FixedPcdGet64 (PcdSunxiMmcBase);
  mIndex = FixedPcdGet32 (PcdSunxiMmcIndex);
  DEBUG ((DEBUG_INFO, "SunxiMmc: SMHC%u @ 0x%lx\n", mIndex, (UINT64)mBase));

  Handle = NULL;
  return gBS->InstallMultipleProtocolInterfaces (
                &Handle,
                &gEmbeddedMmcHostProtocolGuid,
                &mMmcHost,
                &gEfiDevicePathProtocolGuid,
                &mDevicePath,
                NULL
                );
}
