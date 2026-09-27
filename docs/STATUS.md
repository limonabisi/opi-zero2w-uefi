# Orange Pi Zero 2W

> [!NOTE]
> Firmware is loaded from the microSD card (flash `opi-zero2w-edk2-sd.img`). SPI flash boot is not supported.

**Preview**
State: Active
Codename: opizero2w
SoC: Allwinner H618 (4x Cortex-A53, 1 GB LPDDR4)
Version: 0.11.2-h618

Contributors: limonabisi

Legend: ✅ Working · ⚠️ Partially working · ❌ Not working · ❓ Not tested yet

## UEFI Status

| Feature | Description | State |
|---|---|---|
| Display (HDMI) | 1920x1080 | ✅ |
| HDMI EDID | DDC does not respond, fixed 1920x1080 | ⚠️ |
| microSD | Read / write | ✅ |
| USB Host Mode | USB1 port, EHCI + OHCI, with and without hub | ✅ |
| USB Device Mode | USB0 (OTG on the power port), no driver | ❌ |
| Mass Storage | USB drives, boot from USB | ✅ |
| USB Keyboard | | ✅ |
| Serial Console | UART0 115200 8N1, output only | ✅ |
| RTC | Keeps time across reboots, no battery | ⚠️ |
| CPU Frequency | Up to the speed-bin maximum (1416 MHz on bin 0) | ✅ |
| Temperature Sensor | Shown in setup | ✅ |
| Graphical Setup | F2 = setup, ESC = boot menu | ✅ |
| Persistent Variables | Stored on the microSD card | ✅ |
| Secure Boot | Microsoft KEK/db, enabled from setup | ✅ |
| Virtualization | OS started at EL2, GICv2 virtualization | ✅ |
| ACPI Tables | FADT, MADT, GTDT, DSDT, DBG2, SPCR | ✅ |
| SMBIOS | | ✅ |
| FAT | | ✅ |
| Ext4 | Driver included | ❓ |
| SPI Flash | Boot from SPI not supported | ❌ |
| Network Boot | No Ethernet on the board, no USB NIC driver | ❌ |
| TPM | No hardware TPM | ❌ |
| Windows Boot | Windows 10 ARM64 from a USB drive | ✅ |
| x64 Engine | Whole x86-64 PC: boots x86-64 Linux ISOs (preview, no network, slow) | ⚠️ |
| Linux Boot | Needs Device Tree mode | ❌ |

## OS Status

### Windows

| Feature | Description | State |
|---|---|---|
| Boot | Windows 10 22H2 ARM64, from a USB drive | ✅ |
| CPU | 4 cores via PSCI | ✅ |
| Display | Framebuffer only (Basic Display) | ✅ |
| USB Host Mode | Inbox EHCI / OHCI drivers, keyboard and USB drive | ✅ |
| Mass Storage | Windows must be installed on a USB drive | ✅ |
| microSD | Controller is not SDHCI, no driver | ❌ |
| USB Device Mode | | ❌ |
| WLAN | | ❌ |
| Bluetooth | | ❌ |
| GPU | Mali-G31, no driver | ❌ |
| HDMI Audio | | ❌ |
| Temperature Sensor | | ❌ |
| CPU Frequency Scaling | Fixed at the speed set by the firmware | ❌ |
| RTC | | ❓ |

### Linux

| Feature | Description | State |
|---|---|---|
| Boot | Needs Device Tree mode (ACPI only for now) | ❌ |
