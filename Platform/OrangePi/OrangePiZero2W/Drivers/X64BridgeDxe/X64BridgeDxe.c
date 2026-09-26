/** @file
  x64 Bridge: when enabled in setup ("X64Bridge" variable), start the x86-64
  UEFI emulator (intel/MultiArchUefiPkg EmulatorDxe, bundled in the FV as a
  plain file) so x86-64 UEFI applications, drivers and boot loaders run on
  this AArch64 firmware.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/SunxiCpuThermal.h>

#ifndef X64_BRIDGE_DEFAULT
#define X64_BRIDGE_DEFAULT  0
#endif

// FV file holding EmulatorDxe.efi (see OrangePiZero2W.fdf)
STATIC EFI_GUID  mEmulatorFile = {
  0x5c2a7e41, 0x93b6, 0x4f0d, { 0x8e, 0x2a, 0x1b, 0x7c, 0x9d, 0x4e, 0x6f, 0x30 }
};

STATIC
EFI_DEVICE_PATH_PROTOCOL *
FindEmulatorFile (
  VOID
  )
{
  EFI_HANDLE                         *Handles;
  UINTN                              Count;
  UINTN                              Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL      *Fv;
  UINTN                              Size;
  EFI_FV_FILETYPE                    Type;
  EFI_FV_FILE_ATTRIBUTES             Attr;
  UINT32                             Auth;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  Node;
  EFI_DEVICE_PATH_PROTOCOL           *Dp;

  Dp = NULL;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiFirmwareVolume2ProtocolGuid, NULL, &Count, &Handles))) {
    return NULL;
  }

  for (Index = 0; Index < Count && Dp == NULL; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiFirmwareVolume2ProtocolGuid, (VOID **)&Fv))) {
      continue;
    }

    Size = 0;
    if (EFI_ERROR (Fv->ReadFile (Fv, &mEmulatorFile, NULL, &Size, &Type, &Attr, &Auth))) {
      continue;
    }

    EfiInitializeFwVolDevicepathNode (&Node, &mEmulatorFile);
    Dp = AppendDevicePathNode (DevicePathFromHandle (Handles[Index]), (EFI_DEVICE_PATH_PROTOCOL *)&Node);
  }

  FreePool (Handles);
  return Dp;
}

EFI_STATUS
EFIAPI
X64BridgeDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  STATIC EFI_GUID           SetupGuid = SUNXI_SETUP_VARIABLE_GUID;
  UINT8                     Enabled;
  UINTN                     Size;
  EFI_DEVICE_PATH_PROTOCOL  *Dp;
  EFI_HANDLE                Emulator;
  EFI_STATUS                Status;

  Size    = sizeof (Enabled);
  Enabled = X64_BRIDGE_DEFAULT;
  gRT->GetVariable (L"X64Bridge", &SetupGuid, NULL, &Size, &Enabled);
  if (Enabled == 0) {
    DEBUG ((DEBUG_INFO, "x64 Bridge: disabled\n"));
    return EFI_SUCCESS;
  }

  Dp = FindEmulatorFile ();
  if (Dp == NULL) {
    DEBUG ((DEBUG_ERROR, "x64 Bridge: emulator not found in the firmware volume\n"));
    return EFI_SUCCESS;
  }

  Status = gBS->LoadImage (FALSE, ImageHandle, Dp, NULL, 0, &Emulator);
  if (!EFI_ERROR (Status)) {
    Status = gBS->StartImage (Emulator, NULL, NULL);
  }

  DEBUG ((DEBUG_ERROR, "x64 Bridge: x86-64 UEFI emulator started: %r\n", Status));
  FreePool (Dp);
  return EFI_SUCCESS;
}
