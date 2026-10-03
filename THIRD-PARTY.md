# Third-party components

| Component | Where | License |
|---|---|---|
| TianoCore EDK2 (upstream) | cloned at build time, `patches/edk2-*.patch` | BSD-2-Clause-Patent |
| OHCI driver (from the EDK2 Quark platform) | `Silicon/Allwinner/H616Pkg/Drivers/OhciDxe` | BSD-2-Clause-Patent, Copyright (c) 2013-2016 Intel Corporation |
| SD-backed variable store (based on the Raspberry Pi 4 port) | `Platform/OrangePi/OrangePiZero2W/Drivers/VarBlockServiceDxe` | BSD-2-Clause-Patent, Copyright (c) 2018 Andrei Warkentin |
| Poppins font (rendered to bitmaps in `Assets.h`) | `Platform/OrangePi/OrangePiZero2W/Applications/OpiSetup` | SIL Open Font License 1.1, see `OFL.txt` there |
| DejaVu Sans Mono (rendered to 8x16 bitmaps in `font8x16.h`, the x64 Engine's F11 counters) | `X64Engine/font8x16.h` | Bitstream Vera Fonts licence / public domain (DejaVu changes) |
| Microsoft Secure Boot certificates (KEK, db) | `Platform/OrangePi/OrangePiZero2W/SecureBootKeys` | Public certificates from [microsoft/secureboot_objects](https://github.com/microsoft/secureboot_objects) |
| MultiArchUefiPkg (build setup and libraries for the x64 Engine) | `Platform/OrangePi/OrangePiZero2W/Binaries/X64Engine` (binary), source: [intel/MultiArchUefiPkg](https://github.com/intel/MultiArchUefiPkg) @ `ea23ded14ba2` + `patches/multiarchuefipkg-0001-opi-zero2w.patch` | LGPL-2.1 |
| unicorn-for-efi (CPU core of the x64 Engine) | source: [intel/unicorn-for-efi](https://github.com/intel/unicorn-for-efi) @ `40a8d07ba3ce` + `patches/unicorn-0001-cpuid-fpu-tsc.patch`, `patches/unicorn-0002-x64-engine-system-mode.patch` | GPL-2.0 |
| Trusted Firmware-A v2.13.0 (BL31) | cloned at build time, `patches/tf-a-0001-sunxi-edk2-bl33.patch` | BSD-3-Clause |
| U-Boot v2025.07 (SPL only: DRAM and PMIC init) | cloned at build time, `orangepi_zero2w_defconfig` | GPL-2.0+ |

The prebuilt SD card images in the Releases contain U-Boot SPL and TF-A BL31 binaries built
from the unmodified upstream tags above (TF-A with the patch in `patches/`).
`scripts/build-all.sh` fetches and builds the exact sources; `scripts/build-x64engine.sh`
rebuilds `X64Engine.efi` from the pinned commits and patches above.

Register values and hardware sequences for the H616/H618 peripherals follow mainline
Linux and U-Boot.
