/** @file
  ArmPlatformLib for the Orange Pi Zero 2W (Allwinner H618).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/ArmLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/SerialPortLib.h>
#include <Ppi/ArmMpCoreInfo.h>

#include <H616.h>

STATIC ARM_CORE_INFO  mCoreInfo[] = {
  { 0x0, 0, 0, 0, (UINT64)0xFFFFFFFF },
  { 0x1, 0, 0, 0, (UINT64)0xFFFFFFFF },
  { 0x2, 0, 0, 0, (UINT64)0xFFFFFFFF },
  { 0x3, 0, 0, 0, (UINT64)0xFFFFFFFF },
};

EFI_BOOT_MODE
ArmPlatformGetBootMode (
  VOID
  )
{
  return BOOT_WITH_FULL_CONFIGURATION;
}

STATIC CONST CHAR8  mBanner[] =
  "\r\n[EDK2] Orange Pi Zero 2W (H618) - UEFI firmware starting\r\n";

RETURN_STATUS
ArmPlatformInitialize (
  IN  UINTN  MpId
  )
{
  //
  // U-Boot SPL has already muxed PH0/PH1 to UART0 and enabled its clock,
  // and TF-A BL31 has been printing on it, so the console just works.
  //
  SerialPortInitialize ();
  SerialPortWrite ((UINT8 *)mBanner, sizeof (mBanner) - 1);
  return RETURN_SUCCESS;
}

STATIC
EFI_STATUS
GetMpCoreInfo (
  OUT UINTN          *CoreCount,
  OUT ARM_CORE_INFO  **ArmCoreTable
  )
{
  *CoreCount    = ARRAY_SIZE (mCoreInfo);
  *ArmCoreTable = mCoreInfo;
  return EFI_SUCCESS;
}

STATIC ARM_MP_CORE_INFO_PPI    mMpCoreInfoPpi = { GetMpCoreInfo };
STATIC EFI_PEI_PPI_DESCRIPTOR  mPlatformPpiTable[] = {
  {
    EFI_PEI_PPI_DESCRIPTOR_PPI | EFI_PEI_PPI_DESCRIPTOR_TERMINATE_LIST,
    &gArmMpCoreInfoPpiGuid,
    &mMpCoreInfoPpi
  }
};

VOID
ArmPlatformGetPlatformPpiList (
  OUT UINTN                   *PpiListSize,
  OUT EFI_PEI_PPI_DESCRIPTOR  **PpiList
  )
{
  *PpiListSize = sizeof (mPlatformPpiTable);
  *PpiList     = mPlatformPpiTable;
}

//
// Memory map:
//   0x0000_0000 - 0x3FFF_FFFF  SRAM + peripherals            -> Device
//   0x4000_0000 - 0x4003_FFFF  TF-A BL31 (secure monitor)     -> not mapped
//   0x4004_0000 - top of DRAM  DRAM (PcdSystemMemoryBase/Size) -> Write-back
//
#define MAX_REGIONS  3

STATIC ARM_MEMORY_REGION_DESCRIPTOR  mMemoryTable[MAX_REGIONS];

VOID
ArmPlatformGetVirtualMemoryMap (
  IN ARM_MEMORY_REGION_DESCRIPTOR  **VirtualMemoryMap
  )
{
  UINTN  Index;

  Index = 0;

  // SoC peripherals and SRAM
  mMemoryTable[Index].PhysicalBase = 0x00000000;
  mMemoryTable[Index].VirtualBase  = 0x00000000;
  mMemoryTable[Index].Length       = H616_DRAM_BASE;
  mMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;
  Index++;

  // DRAM usable by UEFI / OS
  mMemoryTable[Index].PhysicalBase = PcdGet64 (PcdSystemMemoryBase);
  mMemoryTable[Index].VirtualBase  = PcdGet64 (PcdSystemMemoryBase);
  mMemoryTable[Index].Length       = PcdGet64 (PcdSystemMemorySize);
  mMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
  Index++;

  // End of table
  ZeroMem (&mMemoryTable[Index], sizeof (ARM_MEMORY_REGION_DESCRIPTOR));

  DEBUG ((
    DEBUG_INFO,
    "OPiZ2W: DRAM 0x%lx - 0x%lx (BL31 hole at 0x%lx)\n",
    PcdGet64 (PcdSystemMemoryBase),
    PcdGet64 (PcdSystemMemoryBase) + PcdGet64 (PcdSystemMemorySize) - 1,
    H616_BL31_BASE
    ));

  *VirtualMemoryMap = mMemoryTable;
}
