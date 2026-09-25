/** @file
  Persistent UEFI variables for the Orange Pi Zero 2W.

  There is no SPI flash on the board: the whole firmware (FD) lives on the
  microSD card, inside the FIT image that U-Boot SPL loads.  The FD contains
  a 128 KiB NV store (variables + FTW), which VarBlockService.c keeps in
  runtime memory and exposes as a memory-mapped FVB.

  This file writes the NV store back to the very same place on the card it
  was loaded from, so the next boot starts with the saved variables:

    SD byte 8 KiB : eGON SPL, header word 0x10 = SPL length
    SD 8 KiB + SPL length : FIT header (FDT); the "uboot" image (= our FD)
                     is external data at ALIGN(totalsize, 4) + data-offset
    FD + (NV base - FD base) : the NV store

  The location is verified by comparing the first sector of the FD on the
  card with the FD in memory before anything is written.

  The store is flushed (only if it changed) at ReadyToBoot, whenever an
  image is loaded after ReadyToBoot, and on ResetSystem(); like the
  Raspberry Pi port this is based on.

  Copyright (c) 2018, Andrei Warkentin <andrey.warkentin@gmail.com>
  Copyright (C) 2015, Red Hat, Inc.
  Copyright (c) 2006-2014, Intel Corporation. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "VarBlockService.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Protocol/ResetNotification.h>

#define SD_SPL_OFFSET     SIZE_8KB
#define SD_SECTOR         512
#define FIT_MAX_HEADER    SIZE_64KB

STATIC BOOLEAN  mStoreSearched;
STATIC UINT8    *mOnDisk;       // copy of the NV store as it is on the card

#define SAVE_CHUNK  SIZE_4KB
STATIC BOOLEAN  mExitBootServices;

VOID
InstallProtocolInterfaces (
  IN EFI_FW_VOL_BLOCK_DEVICE  *FvbDevice
  )
{
  EFI_STATUS                          Status;
  EFI_HANDLE                          FwbHandle;
  EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *OldFwbInterface;

  Status = gBS->LocateDevicePath (
                  &gEfiFirmwareVolumeBlockProtocolGuid,
                  &FvbDevice->DevicePath,
                  &FwbHandle
                  );
  if (EFI_ERROR (Status)) {
    FwbHandle = NULL;
    Status    = gBS->InstallMultipleProtocolInterfaces (
                       &FwbHandle,
                       &gEfiFirmwareVolumeBlockProtocolGuid,
                       &FvbDevice->FwVolBlockInstance,
                       &gEfiDevicePathProtocolGuid,
                       FvbDevice->DevicePath,
                       &gEdkiiNvVarStoreFormattedGuid,
                       NULL,
                       NULL
                       );
    ASSERT_EFI_ERROR (Status);
  } else if (IsDevicePathEnd (FvbDevice->DevicePath)) {
    Status = gBS->HandleProtocol (
                    FwbHandle,
                    &gEfiFirmwareVolumeBlockProtocolGuid,
                    (VOID **)&OldFwbInterface
                    );
    ASSERT_EFI_ERROR (Status);

    Status = gBS->ReinstallProtocolInterface (
                    FwbHandle,
                    &gEfiFirmwareVolumeBlockProtocolGuid,
                    OldFwbInterface,
                    &FvbDevice->FwVolBlockInstance
                    );
    ASSERT_EFI_ERROR (Status);
  } else {
    ASSERT (FALSE);
  }
}

STATIC
VOID
EFIAPI
FvbVirtualAddressChangeEvent (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EfiConvertPointer (0x0, (VOID **)&mFvInstance->FvBase);
  EfiConvertPointer (0x0, (VOID **)&mFvInstance->VolumeHeader);
  EfiConvertPointer (0x0, (VOID **)&mFvInstance);
}

VOID
InstallVirtualAddressChangeHandler (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   VirtualAddressChangeEvent;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  FvbVirtualAddressChangeEvent,
                  NULL,
                  &gEfiEventVirtualAddressChangeGuid,
                  &VirtualAddressChangeEvent
                  );
  ASSERT_EFI_ERROR (Status);
}

STATIC
EFI_STATUS
ReadBytes (
  IN  EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN  UINT64                 Offset,
  IN  UINTN                  Size,
  OUT VOID                   *Buffer
  )
{
  if (((Offset % SD_SECTOR) != 0) || ((Size % SD_SECTOR) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  return BlockIo->ReadBlocks (
                    BlockIo,
                    BlockIo->Media->MediaId,
                    Offset / SD_SECTOR,
                    Size,
                    Buffer
                    );
}

/**
  Check whether this (whole-card) BlockIo holds the firmware we booted from
  and return the byte offset of our NV store on it.
**/
STATIC
EFI_STATUS
LocateStoreOnDisk (
  IN  EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  OUT UINT64                 *NvOffset
  )
{
  EFI_STATUS    Status;
  UINT8         *Buf;
  UINT32        SplLength;
  UINT64        FitOffset;
  UINT32        FitSize;
  INT32         Node;
  INT32         Len;
  CONST UINT32  *Prop;
  UINT64        FdOffset;
  UINT32        FdSize;

  Buf = AllocatePool (FIT_MAX_HEADER);
  if (Buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  // eGON header of the SPL
  Status = ReadBytes (BlockIo, SD_SPL_OFFSET, SD_SECTOR, Buf);
  if (EFI_ERROR (Status) || (CompareMem (Buf + 4, "eGON.BT0", 8) != 0)) {
    Status = EFI_NOT_FOUND;
    goto Done;
  }

  SplLength = ReadUnaligned32 ((UINT32 *)(Buf + 0x10));
  FitOffset = SD_SPL_OFFSET + SplLength;
  if ((SplLength == 0) || (SplLength > SIZE_1MB) || ((FitOffset % SD_SECTOR) != 0)) {
    Status = EFI_NOT_FOUND;
    goto Done;
  }

  // FIT header
  Status = ReadBytes (BlockIo, FitOffset, SD_SECTOR, Buf);
  if (EFI_ERROR (Status) || (FdtCheckHeader (Buf) != 0)) {
    Status = EFI_NOT_FOUND;
    goto Done;
  }

  FitSize = SwapBytes32 (ReadUnaligned32 ((UINT32 *)(Buf + 4)));   // totalsize
  if ((FitSize < SD_SECTOR) || (FitSize > FIT_MAX_HEADER)) {
    Status = EFI_NOT_FOUND;
    goto Done;
  }

  Status = ReadBytes (BlockIo, FitOffset, ALIGN_VALUE (FitSize, SD_SECTOR), Buf);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Node = FdtPathOffset (Buf, "/images/uboot");
  Prop = (Node >= 0) ? FdtGetProp (Buf, Node, "data-offset", &Len) : NULL;
  if ((Prop == NULL) || (Len != sizeof (UINT32))) {
    Status = EFI_NOT_FOUND;
    goto Done;
  }

  FdOffset = FitOffset + ALIGN_VALUE (FitSize, 4) + Fdt32ToCpu (*Prop);
  Prop     = FdtGetProp (Buf, Node, "data-size", &Len);
  FdSize   = ((Prop != NULL) && (Len == sizeof (UINT32))) ? Fdt32ToCpu (*Prop) : 0;
  if (((FdOffset % SD_SECTOR) != 0) ||
      (FdSize < mFvInstance->Offset + mFvInstance->FvLength))
  {
    Status = EFI_NOT_FOUND;
    goto Done;
  }

  // The FD on the card must be the one we are running
  Status = ReadBytes (BlockIo, FdOffset, SD_SECTOR, Buf);
  if (EFI_ERROR (Status) ||
      (CompareMem (Buf, (VOID *)(UINTN)FixedPcdGet64 (PcdFdBaseAddress), SD_SECTOR) != 0))
  {
    DEBUG ((DEBUG_WARN, "NvVarStore: FD on the card differs from the running one, not saving\n"));
    Status = EFI_NOT_FOUND;
    goto Done;
  }

  *NvOffset = FdOffset + mFvInstance->Offset;
  Status    = EFI_SUCCESS;

Done:
  FreePool (Buf);
  return Status;
}

STATIC
VOID
FindStore (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_HANDLE             *Handles;
  UINTN                  Count;
  UINTN                  Index;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  UINT64                 NvOffset;

  if (mFvInstance->BlockIo != NULL) {
    return;
  }

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (EFI_ERROR (Status) ||
        BlockIo->Media->LogicalPartition ||
        !BlockIo->Media->MediaPresent ||
        BlockIo->Media->ReadOnly ||
        (BlockIo->Media->BlockSize != SD_SECTOR))
    {
      continue;
    }

    if (!EFI_ERROR (LocateStoreOnDisk (BlockIo, &NvOffset))) {
      mOnDisk = AllocatePool (mFvInstance->FvLength);
      if ((mOnDisk == NULL) ||
          EFI_ERROR (ReadBytes (BlockIo, NvOffset, mFvInstance->FvLength, mOnDisk)))
      {
        DEBUG ((DEBUG_ERROR, "NvVarStore: cannot read the NV store from the card\n"));
        break;
      }

      mFvInstance->BlockIo    = BlockIo;
      mFvInstance->DiskOffset = NvOffset;
      DEBUG ((DEBUG_INFO, "NvVarStore: saving variables to the SD card at byte 0x%lx\n", NvOffset));
      break;
    }
  }

  FreePool (Handles);

  if ((mFvInstance->BlockIo == NULL) && !mStoreSearched) {
    DEBUG ((DEBUG_WARN, "NvVarStore: boot card not found, variables will not be saved\n"));
  }

  mStoreSearched = TRUE;
}

STATIC
VOID
DumpVars (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Chunk;
  UINTN       Written;
  UINT8       *Mem;

  if (!mFvInstance->Dirty || mExitBootServices) {
    return;
  }

  FindStore ();
  if ((mFvInstance->BlockIo == NULL) || (mOnDisk == NULL)) {
    return;
  }

  //
  // Only rewrite the 4 KiB chunks that differ from what is on the card
  // (usually a few sectors per boot), and read each one back to verify.
  //
  Mem     = (UINT8 *)mFvInstance->FvBase;
  Written = 0;
  Status  = EFI_SUCCESS;
  for (Chunk = 0; Chunk < mFvInstance->FvLength; Chunk += SAVE_CHUNK) {
    if (CompareMem (Mem + Chunk, mOnDisk + Chunk, SAVE_CHUNK) == 0) {
      continue;
    }

    Status = mFvInstance->BlockIo->WriteBlocks (
                                     mFvInstance->BlockIo,
                                     mFvInstance->BlockIo->Media->MediaId,
                                     (mFvInstance->DiskOffset + Chunk) / SD_SECTOR,
                                     SAVE_CHUNK,
                                     Mem + Chunk
                                     );
    if (!EFI_ERROR (Status)) {
      Status = ReadBytes (mFvInstance->BlockIo, mFvInstance->DiskOffset + Chunk, SAVE_CHUNK, mOnDisk + Chunk);
    }

    if (!EFI_ERROR (Status) && (CompareMem (Mem + Chunk, mOnDisk + Chunk, SAVE_CHUNK) != 0)) {
      Status = EFI_DEVICE_ERROR;
    }

    if (EFI_ERROR (Status)) {
      break;
    }

    Written++;
  }

  if (!EFI_ERROR (Status)) {
    mFvInstance->BlockIo->FlushBlocks (mFvInstance->BlockIo);
    mFvInstance->Dirty = FALSE;
  }

  DEBUG ((
    EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
    "NvVarStore: %u KiB written to SD and verified: %r\n",
    (UINT32)(Written * SAVE_CHUNK / 1024),
    Status
    ));
}

STATIC
VOID
EFIAPI
DumpVarsOnEvent (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  DumpVars ();
}

STATIC
VOID
EFIAPI
DumpVarsOnReset (
  IN EFI_RESET_TYPE  ResetType,
  IN EFI_STATUS      ResetStatus,
  IN UINTN           DataSize,
  IN VOID            *ResetData OPTIONAL
  )
{
  DumpVars ();
}

STATIC
VOID
EFIAPI
OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  // The card driver is gone after this point; nothing can be saved anymore.
  mExitBootServices = TRUE;
}

STATIC
VOID
EFIAPI
ReadyToBootHandler (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   ImageInstallEvent;
  VOID        *ImageRegistration;

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  DumpVarsOnEvent,
                  NULL,
                  &ImageInstallEvent
                  );
  ASSERT_EFI_ERROR (Status);

  Status = gBS->RegisterProtocolNotify (
                  &gEfiLoadedImageProtocolGuid,
                  ImageInstallEvent,
                  &ImageRegistration
                  );
  ASSERT_EFI_ERROR (Status);

  DumpVars ();
  gBS->CloseEvent (Event);
}

STATIC
VOID
EFIAPI
OnResetNotificationInstall (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS                       Status;
  EFI_RESET_NOTIFICATION_PROTOCOL  *ResetNotify;

  Status = gBS->LocateProtocol (&gEfiResetNotificationProtocolGuid, NULL, (VOID **)&ResetNotify);
  if (!EFI_ERROR (Status)) {
    ResetNotify->RegisterResetNotify (ResetNotify, DumpVarsOnReset);
    gBS->CloseEvent (Event);
  }
}

VOID
InstallDumpVarEventHandlers (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   Event;
  VOID        *Registration;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  ReadyToBootHandler,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &Event
                  );
  ASSERT_EFI_ERROR (Status);

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  OnExitBootServices,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &Event
                  );
  ASSERT_EFI_ERROR (Status);

  //
  // We are dispatched before ResetSystemRuntimeDxe (a priori), so hook the
  // reset notification as soon as it shows up.
  //
  Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnResetNotificationInstall, NULL, &Event);
  ASSERT_EFI_ERROR (Status);
  Status = gBS->RegisterProtocolNotify (&gEfiResetNotificationProtocolGuid, Event, &Registration);
  ASSERT_EFI_ERROR (Status);
  gBS->SignalEvent (Event);
}
