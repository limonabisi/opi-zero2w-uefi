# Secure Boot default keys

Enrolled by OpiSetup (Security > Secure Boot) from the firmware volume.

- `PK.der`: platform key for this port (self-signed; the private key was not
  kept, so PK changes go through setup mode, i.e. Secure Boot off/on in setup).
- `KEK/`, `db/`: Microsoft certificates from
  https://github.com/microsoft/secureboot_objects (PreSignedObjects):
  KEK CA 2011 + KEK 2K CA 2023; Windows Production PCA 2011, UEFI CA 2011,
  Windows UEFI CA 2023, UEFI CA 2023, Option ROM UEFI CA 2023.

No dbx is enrolled by default (revoking PCA 2011 would block Windows 10 media).
