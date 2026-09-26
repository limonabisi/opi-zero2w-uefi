/** @file
  OpiSetup: turn UEFI Secure Boot on (enroll the built-in PK, Microsoft KEK
  and db certificates from the firmware volume) or off (delete the PK, which
  puts the platform back into setup mode).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "OpiSetup.h"

#include <Guid/AuthenticatedVariableFormat.h>
#include <Guid/GlobalVariable.h>
#include <Guid/ImageAuthentication.h>
#include <Pi/PiFirmwareFile.h>
#include <Pi/PiFirmwareVolume.h>
#include <Library/DxeServicesLib.h>
#include <UefiSecureBoot.h>
#include <Library/SecureBootVariableLib.h>

#define MAX_CERTS  8

STATIC
EFI_STATUS
EnrollFromFv (
  IN CHAR16    *Name,
  IN EFI_GUID  *VendorGuid,
  IN EFI_GUID  *FileGuid
  )
{
  SECURE_BOOT_CERTIFICATE_INFO  Certs[MAX_CERTS];
  UINTN                         Count;
  VOID                          *Data;
  UINTN                         Size;
  EFI_SIGNATURE_LIST            *List;
  UINTN                         ListSize;
  EFI_STATUS                    Status;
  UINTN                         Index;

  for (Count = 0; Count < MAX_CERTS; Count++) {
    if (EFI_ERROR (GetSectionFromAnyFv (FileGuid, EFI_SECTION_RAW, Count, &Data, &Size))) {
      break;
    }

    Certs[Count].Data     = Data;
    Certs[Count].DataSize = Size;
  }

  if (Count == 0) {
    return EFI_NOT_FOUND;
  }

  Status = SecureBootCreateDataFromInput (&ListSize, &List, Count, Certs);
  if (!EFI_ERROR (Status)) {
    Status = EnrollFromInput (Name, VendorGuid, ListSize, List);
    FreePool (List);
  }

  for (Index = 0; Index < Count; Index++) {
    FreePool ((VOID *)Certs[Index].Data);
  }

  DEBUG ((DEBUG_INFO, "OpiSetup: enroll %s (%u certs): %r\n", Name, Count, Status));
  return Status;
}

BOOLEAN
SbPkEnrolled (
  VOID
  )
{
  UINTN       Size;
  EFI_STATUS  Status;

  Size   = 0;
  Status = gRT->GetVariable (EFI_PLATFORM_KEY_NAME, &gEfiGlobalVariableGuid, NULL, &Size, NULL);
  return Status == EFI_BUFFER_TOO_SMALL;
}

BOOLEAN
SbActive (
  VOID
  )
{
  UINT8  Value;
  UINTN  Size;

  Size  = 1;
  Value = 0;
  gRT->GetVariable (EFI_SECURE_BOOT_MODE_NAME, &gEfiGlobalVariableGuid, NULL, &Size, &Value);
  return Value == SECURE_BOOT_MODE_ENABLE;
}

EFI_STATUS
SbEnable (
  VOID
  )
{
  EFI_STATUS  Status;

  // start from a clean slate (custom mode), then db, KEK and finally PK
  DeleteSecureBootVariables ();

  Status = EnrollFromFv (EFI_IMAGE_SECURITY_DATABASE, &gEfiImageSecurityDatabaseGuid, &gDefaultdbFileGuid);
  if (!EFI_ERROR (Status)) {
    Status = EnrollFromFv (EFI_KEY_EXCHANGE_KEY_NAME, &gEfiGlobalVariableGuid, &gDefaultKEKFileGuid);
  }

  if (!EFI_ERROR (Status)) {
    Status = EnrollFromFv (EFI_PLATFORM_KEY_NAME, &gEfiGlobalVariableGuid, &gDefaultPKFileGuid);
  }

  SetSecureBootMode (STANDARD_SECURE_BOOT_MODE);
  return Status;
}

EFI_STATUS
SbDisable (
  VOID
  )
{
  return DeletePlatformKey ();
}
