/** @file
  Empty NetLib, see NetLibNull.inf. SnpDxe only takes its unload handler.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

EFI_STATUS
EFIAPI
NetLibDefaultUnload (
  IN EFI_HANDLE  ImageHandle
  )
{
  return EFI_UNSUPPORTED;                  // stays loaded
}
