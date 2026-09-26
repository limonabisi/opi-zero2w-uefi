/** @file
  SMBIOS tables for the Orange Pi Zero 2W (Allwinner H618).

  The UEFI setup front page (UiApp) shows:
    - Type 1 ProductName   -> computer model
    - Type 4 Version/Speed -> CPU model and clock
    - Type 0 BiosVersion   -> firmware version
    - Type 19              -> memory size
  Linux also exposes these under /sys/firmware/dmi.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <IndustryStandard/SmBios.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/Smbios.h>
#include <Protocol/SunxiCpuThermal.h>

#include <H616.h>

#define H616_CCU_PLL_CPUX  0x000

STATIC EFI_SMBIOS_PROTOCOL  *mSmbios;

/**
  Append a record followed by its string set (NUL separated, double NUL end).
**/
STATIC
EFI_STATUS
AddRecord (
  IN VOID         *Record,
  IN CONST CHAR8  **Strings,
  IN UINTN        StringCount
  )
{
  EFI_SMBIOS_TABLE_HEADER  *Hdr;
  UINTN                    Size;
  UINTN                    Index;
  UINT8                    *Buffer;
  UINT8                    *Ptr;
  EFI_SMBIOS_HANDLE        Handle;
  EFI_STATUS               Status;

  Hdr  = (EFI_SMBIOS_TABLE_HEADER *)Record;
  Size = Hdr->Length;
  for (Index = 0; Index < StringCount; Index++) {
    Size += AsciiStrSize (Strings[Index]);
  }

  Size += (StringCount == 0) ? 2 : 1;

  Buffer = AllocateZeroPool (Size);
  if (Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Buffer, Record, Hdr->Length);
  Ptr = Buffer + Hdr->Length;
  for (Index = 0; Index < StringCount; Index++) {
    AsciiStrCpyS ((CHAR8 *)Ptr, AsciiStrSize (Strings[Index]), Strings[Index]);
    Ptr += AsciiStrSize (Strings[Index]);
  }

  Handle = SMBIOS_HANDLE_PI_RESERVED;
  Status = mSmbios->Add (mSmbios, NULL, &Handle, (EFI_SMBIOS_TABLE_HEADER *)Buffer);
  FreePool (Buffer);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "PlatformSmbios: type %u: %r\n", Hdr->Type, Status));
  }

  return Status;
}

STATIC
UINT16
GetCpuMhz (
  VOID
  )
{
  UINT32  Val;
  UINT32  N;
  UINT32  P;

  Val = MmioRead32 (H616_CCU_BASE + H616_CCU_PLL_CPUX);
  N   = ((Val >> 8) & 0xFF) + 1;
  P   = (Val >> 16) & 0x3;
  return (UINT16)((24 * N) >> P);
}

STATIC
VOID
AddType0 (
  VOID
  )
{
  SMBIOS_TABLE_TYPE0  T;
  CONST CHAR8         *S[] = {
    "TianoCore EDK2 (Orange Pi Zero 2W port)",
    "0.3-h618",
    __DATE__
  };

  ZeroMem (&T, sizeof (T));
  T.Hdr.Type                        = EFI_SMBIOS_TYPE_BIOS_INFORMATION;
  T.Hdr.Length                      = sizeof (T);
  T.Vendor                          = 1;
  T.BiosVersion                     = 2;
  T.BiosReleaseDate                 = 3;
  T.BiosSegment                     = 0;
  T.BiosSize                        = 0x0F;  // (n+1) * 64 KiB = 1 MiB
  T.BiosCharacteristics.PlugAndPlayIsSupported = 1;
  T.BIOSCharacteristicsExtensionBytes[1]       = BIT3; // UEFI supported
  T.SystemBiosMajorRelease          = 0;
  T.SystemBiosMinorRelease          = 3;
  T.EmbeddedControllerFirmwareMajorRelease = 0xFF;
  T.EmbeddedControllerFirmwareMinorRelease = 0xFF;
  AddRecord (&T, S, ARRAY_SIZE (S));
}

STATIC
VOID
AddType1 (
  VOID
  )
{
  SMBIOS_TABLE_TYPE1  T;
  CONST CHAR8         *S[] = {
    "Shenzhen Xunlong (Orange Pi)",
    "Orange Pi Zero 2W",
    "1 GB",
    "Not Specified",
    "Orange Pi Zero",
    "Orange Pi"
  };

  ZeroMem (&T, sizeof (T));
  T.Hdr.Type     = EFI_SMBIOS_TYPE_SYSTEM_INFORMATION;
  T.Hdr.Length   = sizeof (T);
  T.Manufacturer = 1;
  T.ProductName  = 2;
  T.Version      = 3;
  T.SerialNumber = 4;
  T.WakeUpType   = SystemWakeupTypePowerSwitch;
  T.SKUNumber    = 5;
  T.Family       = 6;
  AddRecord (&T, S, ARRAY_SIZE (S));
}

STATIC
VOID
AddType3 (
  VOID
  )
{
  SMBIOS_TABLE_TYPE3  T;
  CONST CHAR8         *S[] = { "Shenzhen Xunlong (Orange Pi)", "Orange Pi Zero 2W" };

  ZeroMem (&T, sizeof (T));
  T.Hdr.Type           = EFI_SMBIOS_TYPE_SYSTEM_ENCLOSURE;
  T.Hdr.Length         = OFFSET_OF (SMBIOS_TABLE_TYPE3, ContainedElements);
  T.Manufacturer       = 1;
  T.Version            = 2;
  T.Type               = MiscChassisEmbeddedPc;
  T.BootupState        = ChassisStateSafe;
  T.PowerSupplyState   = ChassisStateSafe;
  T.ThermalState       = ChassisStateSafe;
  T.SecurityStatus     = ChassisSecurityStatusNone;
  AddRecord (&T, S, ARRAY_SIZE (S));
}

STATIC
VOID
AddType4 (
  VOID
  )
{
  SMBIOS_TABLE_TYPE4  T;
  CONST CHAR8         *S[] = {
    "CPU0",
    "Allwinner",
    "H618: 4x Cortex-A53"
  };
  UINT16                      Mhz;
  UINT16                      MaxMhz;
  SUNXI_CPU_THERMAL_PROTOCOL  *Cpu;

  Mhz    = GetCpuMhz ();
  MaxMhz = 1512;
  if (!EFI_ERROR (gBS->LocateProtocol (&gSunxiCpuThermalProtocolGuid, NULL, (VOID **)&Cpu))) {
    Mhz    = (UINT16)Cpu->CurrentMhz;
    MaxMhz = (UINT16)Cpu->MaxMhz;
  }

  ZeroMem (&T, sizeof (T));
  T.Hdr.Type                 = EFI_SMBIOS_TYPE_PROCESSOR_INFORMATION;
  T.Hdr.Length               = sizeof (T);
  T.Socket                   = 1;
  T.ProcessorType            = CentralProcessor;
  T.ProcessorFamily          = ProcessorFamilyIndicatorFamily2;
  T.ProcessorFamily2         = ProcessorFamilyARMv8;
  T.ProcessorManufacturer    = 2;
  T.ProcessorVersion         = 3;
  T.MaxSpeed                 = MaxMhz;
  T.CurrentSpeed             = Mhz;
  T.ExternalClock            = 24;
  T.Status                   = 0x41;  // socket populated, CPU enabled
  T.ProcessorUpgrade         = ProcessorUpgradeNone;
  T.L1CacheHandle            = 0xFFFF;
  T.L2CacheHandle            = 0xFFFF;
  T.L3CacheHandle            = 0xFFFF;
  T.CoreCount                = 4;
  T.EnabledCoreCount         = 4;
  T.ThreadCount              = 4;
  T.ProcessorCharacteristics = 0x00EC;  // 64-bit, multi-core, exec protection, enhanced virtualization
  T.CoreCount2               = 4;
  T.EnabledCoreCount2        = 4;
  T.ThreadCount2             = 4;
  AddRecord (&T, S, ARRAY_SIZE (S));
}

STATIC
VOID
AddMemory (
  VOID
  )
{
  SMBIOS_TABLE_TYPE16  T16;
  SMBIOS_TABLE_TYPE17  T17;
  SMBIOS_TABLE_TYPE19  T19;
  CONST CHAR8          *S17[] = { "LPDDR4", "BANK 0" };
  UINT64               DramBytes;
  UINT64               DramKiB;

  DramBytes = PcdGet64 (PcdSystemMemoryBase) + PcdGet64 (PcdSystemMemorySize) - H616_DRAM_BASE;
  DramKiB   = DramBytes / SIZE_1KB;

  ZeroMem (&T16, sizeof (T16));
  T16.Hdr.Type               = EFI_SMBIOS_TYPE_PHYSICAL_MEMORY_ARRAY;
  T16.Hdr.Length             = sizeof (T16);
  T16.Location               = MemoryArrayLocationSystemBoard;
  T16.Use                    = MemoryArrayUseSystemMemory;
  T16.MemoryErrorCorrection  = MemoryErrorCorrectionNone;
  T16.MaximumCapacity        = (UINT32)DramKiB;
  T16.MemoryErrorInformationHandle = 0xFFFE;
  T16.NumberOfMemoryDevices  = 1;
  AddRecord (&T16, NULL, 0);

  ZeroMem (&T17, sizeof (T17));
  T17.Hdr.Type               = EFI_SMBIOS_TYPE_MEMORY_DEVICE;
  T17.Hdr.Length             = sizeof (T17);
  T17.MemoryArrayHandle      = 0xFFFE;
  T17.MemoryErrorInformationHandle = 0xFFFE;
  T17.TotalWidth             = 32;
  T17.DataWidth              = 32;
  T17.Size                   = (UINT16)(DramBytes / SIZE_1MB);  // in MB
  T17.FormFactor             = MemoryFormFactorOther;
  T17.DeviceLocator          = 1;
  T17.BankLocator            = 2;
  T17.MemoryType             = MemoryTypeLpddr4;
  T17.Speed                  = 792 * 2;
  T17.ConfiguredMemoryClockSpeed = 792 * 2;
  AddRecord (&T17, S17, ARRAY_SIZE (S17));

  ZeroMem (&T19, sizeof (T19));
  T19.Hdr.Type               = EFI_SMBIOS_TYPE_MEMORY_ARRAY_MAPPED_ADDRESS;
  T19.Hdr.Length             = sizeof (T19);
  T19.StartingAddress        = (UINT32)(H616_DRAM_BASE / SIZE_1KB);
  T19.EndingAddress          = (UINT32)((H616_DRAM_BASE + DramBytes) / SIZE_1KB - 1);
  T19.MemoryArrayHandle      = 0xFFFE;
  T19.PartitionWidth         = 1;
  AddRecord (&T19, NULL, 0);
}

STATIC
VOID
AddType32 (
  VOID
  )
{
  SMBIOS_TABLE_TYPE32  T;

  ZeroMem (&T, sizeof (T));
  T.Hdr.Type    = EFI_SMBIOS_TYPE_SYSTEM_BOOT_INFORMATION;
  T.Hdr.Length  = sizeof (T);
  T.BootStatus  = BootInformationStatusNoError;
  AddRecord (&T, NULL, 0);
}

EFI_STATUS
EFIAPI
PlatformSmbiosDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = gBS->LocateProtocol (&gEfiSmbiosProtocolGuid, NULL, (VOID **)&mSmbios);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  AddType0 ();
  AddType1 ();
  AddType3 ();
  AddType4 ();
  AddMemory ();
  AddType32 ();

  DEBUG ((DEBUG_INFO, "PlatformSmbios: H618 @ %u MHz, tables installed\n", GetCpuMhz ()));
  return EFI_SUCCESS;
}
