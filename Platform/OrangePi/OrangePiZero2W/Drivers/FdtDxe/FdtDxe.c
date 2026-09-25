/** @file
  Installs the Orange Pi Zero 2W device tree (embedded in the firmware
  volume) as the EFI FDT configuration table, so that Linux' EFI stub /
  GRUB / systemd-boot can hand it to the kernel.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Guid/Fdt.h>

#include <H616.h>

// FREEFORM file in the FV that carries the DTB as a RAW section
STATIC EFI_GUID  mDtbFileGuid = {
  0x65f00053, 0xda6b, 0x4367, { 0x8c, 0xcf, 0x13, 0x94, 0x9d, 0xb6, 0x40, 0x27 }
};

#define FDT_EXTRA_SPACE  SIZE_16KB

//
// Green status LED (PC13) as a UART-independent progress indicator:
//   SEC start -> ON, DXE (this driver) -> OFF, ReadyToBoot -> ON
//
STATIC
VOID
StatusLed (
  IN BOOLEAN  On
  )
{
  UINTN  DatReg;

  DatReg = H616_PIO_DAT (H616_PIO_PORT_C);
  if (On) {
    MmioOr32 (DatReg, BIT13);
  } else {
    MmioAnd32 (DatReg, ~(UINT32)BIT13);
  }
}

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  StatusLed (TRUE);
}

STATIC
VOID
FixupMemoryNode (
  IN VOID  *Fdt
  )
{
  INT32   Node;
  UINT64  Reg[2];
  UINT64  DramSize;

  //
  // The whole DRAM (including the BL31 region, which the DT already lists
  // under /reserved-memory as no-map).
  //
  DramSize = PcdGet64 (PcdSystemMemoryBase) + PcdGet64 (PcdSystemMemorySize) - H616_DRAM_BASE;

  Node = FdtPathOffset (Fdt, "/memory");
  if (Node < 0) {
    Node = FdtAddSubnode (Fdt, 0, "memory");
    if (Node < 0) {
      DEBUG ((DEBUG_ERROR, "FdtDxe: cannot add /memory node (%d)\n", Node));
      return;
    }
  }

  FdtSetProp (Fdt, Node, "device_type", "memory", sizeof ("memory"));
  Reg[0] = CpuToFdt64 (H616_DRAM_BASE);
  Reg[1] = CpuToFdt64 (DramSize);
  FdtSetProp (Fdt, Node, "reg", Reg, sizeof (Reg));
}

EFI_STATUS
EFIAPI
FdtDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  VOID        *Dtb;
  UINTN       DtbSize;
  VOID        *Fdt;
  UINTN       FdtSize;
  INT32       Ret;
  INT32       Len;
  CONST CHAR8 *Model;

  EFI_EVENT   ReadyToBootEvent;

  StatusLed (FALSE);
  EfiCreateEventReadyToBootEx (TPL_CALLBACK, OnReadyToBoot, NULL, &ReadyToBootEvent);

  Status = GetSectionFromAnyFv (&mDtbFileGuid, EFI_SECTION_RAW, 0, &Dtb, &DtbSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "FdtDxe: DTB not found in FV: %r\n", Status));
    return Status;
  }

  if (FdtCheckHeader (Dtb) != 0) {
    DEBUG ((DEBUG_ERROR, "FdtDxe: embedded DTB has a bad header\n"));
    return EFI_VOLUME_CORRUPTED;
  }

  FdtSize = FdtTotalSize (Dtb) + FDT_EXTRA_SPACE;
  Fdt     = AllocateReservedPages (EFI_SIZE_TO_PAGES (FdtSize));   // survives ExitBootServices
  if (Fdt == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Ret = FdtOpenInto (Dtb, Fdt, (INT32)FdtSize);
  if (Ret != 0) {
    DEBUG ((DEBUG_ERROR, "FdtDxe: FdtOpenInto failed (%d)\n", Ret));
    FreePages (Fdt, EFI_SIZE_TO_PAGES (FdtSize));
    return EFI_DEVICE_ERROR;
  }

  FixupMemoryNode (Fdt);
  FdtPack (Fdt);

  Model = FdtGetProp (Fdt, 0, "model", &Len);
  DEBUG ((DEBUG_INFO, "FdtDxe: installing DT \"%a\" (%u bytes) @ %p\n",
          (Model != NULL) ? Model : "?", FdtTotalSize (Fdt), Fdt));

  Status = gBS->InstallConfigurationTable (&gFdtTableGuid, Fdt);
  if (EFI_ERROR (Status)) {
    FreePages (Fdt, EFI_SIZE_TO_PAGES (FdtSize));
  }

  return Status;
}
