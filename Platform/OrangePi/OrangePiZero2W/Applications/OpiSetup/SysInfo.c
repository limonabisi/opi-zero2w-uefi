/** @file
  Gather the facts shown on the OpiSetup main page.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "OpiSetup.h"

#include <IndustryStandard/SmBios.h>
#include <Library/DevicePathLib.h>
#include <Library/PcdLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/Smbios.h>

STATIC
CONST CHAR8 *
SmbiosString (
  EFI_SMBIOS_TABLE_HEADER  *Rec,
  UINT8                    Index
  )
{
  CONST CHAR8  *P;

  if (Index == 0) {
    return "";
  }

  P = (CONST CHAR8 *)Rec + Rec->Length;
  while (--Index > 0) {
    P += AsciiStrLen (P) + 1;
  }

  return P;
}

STATIC
EFI_SMBIOS_TABLE_HEADER *
FindRecord (
  EFI_SMBIOS_PROTOCOL  *Smbios,
  UINT8                Type
  )
{
  EFI_SMBIOS_HANDLE        Handle;
  EFI_SMBIOS_TYPE          T;
  EFI_SMBIOS_TABLE_HEADER  *Rec;

  Handle = SMBIOS_HANDLE_PI_RESERVED;
  T      = Type;
  if (EFI_ERROR (Smbios->GetNext (Smbios, &Handle, &T, &Rec, NULL))) {
    return NULL;
  }

  return Rec;
}

STATIC
VOID
FormatSize (
  CHAR16  *Buf,
  UINTN   Size,
  UINT64  Bytes
  )
{
  if (Bytes >= SIZE_1GB) {
    UINT64  Tenths = DivU64x64Remainder (MultU64x32 (Bytes, 10), SIZE_1GB, NULL);
    UnicodeSPrint (Buf, Size, L"%lu.%lu GB", DivU64x32 (Tenths, 10), ModU64x32 (Tenths, 10));
  } else {
    UnicodeSPrint (Buf, Size, L"%lu MB", DivU64x32 (Bytes, SIZE_1MB));
  }
}

STATIC
VOID
CollectStorage (
  SYS_INFO  *Info
  )
{
  EFI_HANDLE                *Handles;
  UINTN                     Count;
  UINTN                     Index;
  EFI_BLOCK_IO_PROTOCOL     *BlockIo;
  EFI_DEVICE_PATH_PROTOCOL  *Dp;
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  CONST CHAR16              *Kind;
  CHAR16                    SizeStr[24];

  Info->StorageCount = 0;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles))) {
    return;
  }

  for (Index = 0; Index < Count && Info->StorageCount < ARRAY_SIZE (Info->Storage); Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo)) ||
        BlockIo->Media->LogicalPartition || !BlockIo->Media->MediaPresent)
    {
      continue;
    }

    Kind = L"microSD";
    Dp   = DevicePathFromHandle (Handles[Index]);
    for (Node = Dp; Node != NULL && !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
      if ((DevicePathType (Node) == MESSAGING_DEVICE_PATH) && (DevicePathSubType (Node) == MSG_USB_DP)) {
        Kind = L"USB";
      }
    }

    FormatSize (SizeStr, sizeof (SizeStr), MultU64x32 (BlockIo->Media->LastBlock + 1, BlockIo->Media->BlockSize));
    UnicodeSPrint (Info->Storage[Info->StorageCount], sizeof (Info->Storage[0]), L"%s  ·  %s", Kind, SizeStr);
    Info->StorageCount++;
  }

  FreePool (Handles);
}

VOID
SysInfoCollect (
  SYS_INFO  *Info
  )
{
  EFI_SMBIOS_PROTOCOL           *Smbios;
  EFI_SMBIOS_TABLE_HEADER       *Rec;
  SMBIOS_TABLE_TYPE4            *T4;
  SMBIOS_TABLE_TYPE17           *T17;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  UINT64                        MemBytes;
  EFI_MEMORY_DESCRIPTOR         *Map;
  UINTN                         MapSize, MapKey, DescSize;
  UINT32                        DescVer;
  UINTN                         Index;

  ZeroMem (Info, sizeof (*Info));
  StrCpyS (Info->Board, ARRAY_SIZE (Info->Board), L"Orange Pi Zero 2W");
  StrCpyS (Info->Soc, ARRAY_SIZE (Info->Soc), L"Allwinner H618");
  StrCpyS (Info->Cpu, ARRAY_SIZE (Info->Cpu), L"4x Cortex-A53");

  if (!EFI_ERROR (gBS->LocateProtocol (&gEfiSmbiosProtocolGuid, NULL, (VOID **)&Smbios))) {
    Rec = FindRecord (Smbios, EFI_SMBIOS_TYPE_SYSTEM_INFORMATION);
    if (Rec != NULL) {
      UnicodeSPrint (Info->Board, sizeof (Info->Board), L"%a", SmbiosString (Rec, ((SMBIOS_TABLE_TYPE1 *)Rec)->ProductName));
    }

    Rec = FindRecord (Smbios, EFI_SMBIOS_TYPE_PROCESSOR_INFORMATION);
    if (Rec != NULL) {
      T4 = (SMBIOS_TABLE_TYPE4 *)Rec;
      UnicodeSPrint (
        Info->Cpu,
        sizeof (Info->Cpu),
        L"%u x Cortex-A53  ·  %u MHz (max %u)",
        T4->CoreCount,
        T4->CurrentSpeed,
        T4->MaxSpeed
        );
    }

    Rec = FindRecord (Smbios, EFI_SMBIOS_TYPE_MEMORY_DEVICE);
    if (Rec != NULL) {
      T17 = (SMBIOS_TABLE_TYPE17 *)Rec;
      UnicodeSPrint (
        Info->Memory,
        sizeof (Info->Memory),
        L"%u MB %s",
        (T17->Size == 0x7FFF) ? T17->ExtendedSize : T17->Size,
        (T17->MemoryType == MemoryTypeLpddr4) ? L"LPDDR4" : L"DRAM"
        );
    }
  }

  if (Info->Memory[0] == 0) {
    // count conventional memory from the memory map
    MemBytes = 0;
    MapSize  = 0;
    Map      = NULL;
    gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescSize, &DescVer);
    MapSize += 8 * sizeof (EFI_MEMORY_DESCRIPTOR);
    Map      = AllocatePool (MapSize);
    if ((Map != NULL) && !EFI_ERROR (gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVer))) {
      for (Index = 0; Index < MapSize / DescSize; Index++) {
        EFI_MEMORY_DESCRIPTOR  *D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Index * DescSize);
        MemBytes += EFI_PAGES_TO_SIZE (D->NumberOfPages);
      }
    }

    UnicodeSPrint (Info->Memory, sizeof (Info->Memory), L"%lu MB", DivU64x32 (MemBytes + SIZE_1MB - 1, SIZE_1MB));
    if (Map != NULL) {
      FreePool (Map);
    }
  }

  UnicodeSPrint (Info->Firmware, sizeof (Info->Firmware), L"%s", (CHAR16 *)PcdGetPtr (PcdFirmwareVersionString));
  UnicodeSPrint (Info->BuildDate, sizeof (Info->BuildDate), L"%a", __DATE__);
  UnicodeSPrint (
    Info->Uefi,
    sizeof (Info->Uefi),
    L"%u.%u  ·  EDK2",
    gST->Hdr.Revision >> 16,
    (gST->Hdr.Revision & 0xFFFF) / 10
    );

  if (!EFI_ERROR (gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop)) ||
      !EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop)))
  {
    UnicodeSPrint (
      Info->Display,
      sizeof (Info->Display),
      L"HDMI  ·  %u x %u",
      Gop->Mode->Info->HorizontalResolution,
      Gop->Mode->Info->VerticalResolution
      );
  }

  CollectStorage (Info);
}
