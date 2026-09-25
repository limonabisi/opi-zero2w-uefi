/** @file
  RealTimeClockLib for the Allwinner H616/H618 RTC block (0x07000000).

  The H616 RTC keeps a linear day counter (RTC_YMD[15:0], days since
  1970-01-01, same convention as the Linux/BSP driver) and hours/minutes/
  seconds in RTC_HMS. Writes are latched by the RTC domain; the
  LOSC_CTRL "access" bits stay set until the value has been taken over.

  The clock survives reboots but, without a backup battery, not a power
  loss. An unset/implausible clock is started at 2026-01-01 00:00.

  The RTC stores local time as handed over by RealTimeClockRuntimeDxe,
  which keeps the time zone / daylight settings itself.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/RealTimeClockLib.h>
#include <Library/TimeBaseLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>

#define H616_RTC_BASE        0x07000000
#define RTC_LOSC_CTRL        0x00
#define   LOSC_ACC_MASK      (BIT9 | BIT8 | BIT7)
#define   LOSC_HMS_ACC       BIT8
#define   LOSC_YMD_ACC       BIT7
#define RTC_YMD              0x10
#define RTC_HMS              0x14

#define DEFAULT_EPOCH_DAYS   20454          // 2026-01-01
#define MAX_EPOCH_DAYS       0xFFFF

STATIC UINTN      mRtcBase = H616_RTC_BASE;
STATIC EFI_EVENT  mRtcVirtualAddrChangeEvent;

STATIC
BOOLEAN
RtcWaitIdle (
  IN UINT32  Mask
  )
{
  UINTN  Timeout;

  for (Timeout = 0; Timeout < 50000; Timeout++) {     // 50 ms
    if ((MmioRead32 (mRtcBase + RTC_LOSC_CTRL) & Mask) == 0) {
      return TRUE;
    }

    MicroSecondDelay (1);
  }

  return FALSE;
}

STATIC
EFI_STATUS
RtcWrite (
  IN UINT32  Days,
  IN UINT32  Hms
  )
{
  if (!RtcWaitIdle (LOSC_ACC_MASK)) {
    return EFI_DEVICE_ERROR;
  }

  MmioWrite32 (mRtcBase + RTC_HMS, Hms);
  if (!RtcWaitIdle (LOSC_HMS_ACC)) {
    return EFI_DEVICE_ERROR;
  }

  MmioWrite32 (mRtcBase + RTC_YMD, Days);
  if (!RtcWaitIdle (LOSC_YMD_ACC)) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC
VOID
RtcRead (
  OUT UINT32  *Days,
  OUT UINT32  *Hms
  )
{
  UINT32  D;
  UINT32  T;

  do {
    D = MmioRead32 (mRtcBase + RTC_YMD);
    T = MmioRead32 (mRtcBase + RTC_HMS);
  } while ((D != MmioRead32 (mRtcBase + RTC_YMD)) || (T != MmioRead32 (mRtcBase + RTC_HMS)));

  *Days = D & 0xFFFF;
  *Hms  = T;
}

EFI_STATUS
EFIAPI
LibGetTime (
  OUT EFI_TIME               *Time,
  OUT EFI_TIME_CAPABILITIES  *Capabilities
  )
{
  UINT32  Days;
  UINT32  Hms;
  UINTN   Epoch;

  if (Time == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  RtcRead (&Days, &Hms);

  Epoch = (UINTN)Days * SEC_PER_DAY +
          ((Hms >> 16) & 0x1F) * SEC_PER_HOUR +
          ((Hms >> 8) & 0x3F) * SEC_PER_MIN +
          (Hms & 0x3F);
  EpochToEfiTime (Epoch, Time);

  if (Capabilities != NULL) {
    Capabilities->Resolution = 1;
    Capabilities->Accuracy   = 50000000;     // 50 ppm, in ppt
    Capabilities->SetsToZero = FALSE;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
LibSetTime (
  IN EFI_TIME  *Time
  )
{
  UINTN  Epoch;
  UINTN  Days;

  if ((Time == NULL) || (Time->Year < 1970)) {
    return EFI_INVALID_PARAMETER;
  }

  Epoch = EfiTimeToEpoch (Time);
  Days  = Epoch / SEC_PER_DAY;
  if (Days > MAX_EPOCH_DAYS) {
    return EFI_UNSUPPORTED;
  }

  return RtcWrite ((UINT32)Days, (Time->Hour << 16) | (Time->Minute << 8) | Time->Second);
}

EFI_STATUS
EFIAPI
LibGetWakeupTime (
  OUT BOOLEAN   *Enabled,
  OUT BOOLEAN   *Pending,
  OUT EFI_TIME  *Time
  )
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
LibSetWakeupTime (
  IN BOOLEAN    Enabled,
  OUT EFI_TIME  *Time
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
VOID
EFIAPI
VirtualNotifyEvent (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EfiConvertPointer (0x0, (VOID **)&mRtcBase);
}

EFI_STATUS
EFIAPI
LibRtcInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT32      Days;
  UINT32      Hms;

  //
  // Declare the RTC block as runtime MMIO (the OS may call GetTime()).
  //
  Status = gDS->AddMemorySpace (
                  EfiGcdMemoryTypeMemoryMappedIo,
                  H616_RTC_BASE,
                  SIZE_4KB,
                  EFI_MEMORY_UC | EFI_MEMORY_RUNTIME | EFI_MEMORY_XP
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gDS->SetMemorySpaceAttributes (H616_RTC_BASE, SIZE_4KB, EFI_MEMORY_UC | EFI_MEMORY_RUNTIME | EFI_MEMORY_XP);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  RtcRead (&Days, &Hms);
  if ((Days < DEFAULT_EPOCH_DAYS) || (((Hms >> 16) & 0x1F) > 23) ||
      (((Hms >> 8) & 0x3F) > 59) || ((Hms & 0x3F) > 59))
  {
    DEBUG ((DEBUG_WARN, "H616Rtc: clock not set (day %u), starting at 2026-01-01\n", Days));
    RtcWrite (DEFAULT_EPOCH_DAYS, 0);
  } else {
    DEBUG ((
      DEBUG_INFO,
      "H616Rtc: day %u, %02u:%02u:%02u\n",
      Days,
      (Hms >> 16) & 0x1F,
      (Hms >> 8) & 0x3F,
      Hms & 0x3F
      ));
  }

  return gBS->CreateEventEx (
                EVT_NOTIFY_SIGNAL,
                TPL_NOTIFY,
                VirtualNotifyEvent,
                NULL,
                &gEfiEventVirtualAddressChangeGuid,
                &mRtcVirtualAddrChangeEvent
                );
}
