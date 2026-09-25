/** @file
  QEMU_TEST builds only: expose the virtio-mmio transports of
  "qemu-system-aarch64 -M virt" so a virtio-blk disk can stand in for the
  microSD card (used to test the SD-backed variable store).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/VirtioMmioDeviceLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>

#define VIRT_MMIO_BASE   0x0A000000
#define VIRT_MMIO_SIZE   0x200
#define VIRT_MMIO_COUNT  32

#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  UINT64                      Base;
  EFI_DEVICE_PATH_PROTOCOL    End;
} VIRTIO_MMIO_DEVICE_PATH;
#pragma pack ()

EFI_STATUS
EFIAPI
QemuVirtioMmioDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINTN                    Index;
  UINTN                    Base;
  EFI_HANDLE               Handle;
  VIRTIO_MMIO_DEVICE_PATH  *Dp;
  EFI_STATUS               Status;
  UINTN                    Pass;

  for (Index = 0; Index < VIRT_MMIO_COUNT; Index++) {
    Base = VIRT_MMIO_BASE + Index * VIRT_MMIO_SIZE;
    if ((MmioRead32 (Base) != 0x74726976) || (MmioRead32 (Base + 8) == 0)) {
      continue;
    }

    Dp = AllocateZeroPool (sizeof (*Dp));
    ASSERT (Dp != NULL);
    Dp->Vendor.Header.Type    = HARDWARE_DEVICE_PATH;
    Dp->Vendor.Header.SubType = HW_VENDOR_DP;
    SetDevicePathNodeLength (&Dp->Vendor, sizeof (Dp->Vendor) + sizeof (UINT64));
    Dp->Vendor.Guid = (EFI_GUID) {
      0x837dca9e, 0xe874, 0x4d82, { 0xb2, 0x9a, 0x23, 0xfe, 0x0e, 0x23, 0xd1, 0xe2 }
    };
    Dp->Base = Base;
    SetDevicePathEndNode (&Dp->End);

    Handle = NULL;
    Status = gBS->InstallProtocolInterface (&Handle, &gEfiDevicePathProtocolGuid, EFI_NATIVE_INTERFACE, Dp);
    if (!EFI_ERROR (Status)) {
      Status = VirtioMmioInstallDevice (Base, Handle);
    }

    DEBUG ((DEBUG_INFO, "QemuVirtio: device %u @ 0x%lx: %r\n", MmioRead32 (Base + 8), (UINT64)Base, Status));
  }

  //
  // "-M virt,highmem=off -device pci-ohci": program BAR0 of any OHCI on
  // PCI bus 0 by hand (ECAM @ 0x3f000000, MMIO window from 0x10000000)
  // and expose it the same way the board exposes its OHCI (non-coherent
  // non-discoverable device), to test H616Pkg OhciDxe.
  //
  for (Pass = 0; Pass < 2; Pass++) {        // EHCI first, then OHCI companions
    for (Index = 0; Index < 32; Index++) {
      UINTN   Cfg;
      UINT32  Bar;
      UINT32  Class;

      Cfg   = 0x3F000000 + (Index << 15);
      Class = MmioRead32 (Cfg + 8) >> 8;
      if ((MmioRead32 (Cfg) == 0xFFFFFFFF) ||
          (Class != ((Pass == 0) ? 0x0C0320U : 0x0C0310U)))
      {
        continue;
      }

      Bar = 0x10000000 + (UINT32)Index * SIZE_64KB;
      MmioWrite32 (Cfg + 0x10, Bar);
      MmioOr16 (Cfg + 4, BIT1 | BIT2);     // memory space + bus master
      Status = RegisterNonDiscoverableMmioDevice (
                 (Pass == 0) ? NonDiscoverableDeviceTypeEhci : NonDiscoverableDeviceTypeOhci,
                 NonDiscoverableDeviceDmaTypeNonCoherent,
                 NULL,
                 NULL,
                 1,
                 (UINTN)Bar,
                 (UINTN)SIZE_4KB
                 );
      DEBUG ((DEBUG_INFO, "QemuTest: PCI %a %u -> 0x%x: %r\n", (Pass == 0) ? "EHCI" : "OHCI", Index, Bar, Status));
    }
  }

  return EFI_SUCCESS;
}
