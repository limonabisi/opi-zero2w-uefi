/** @file
  Allwinner H616/H618 USB host bring-up (EHCI1 on USB port 1).

  On the Orange Pi Zero 2W, USB port 1 is the second USB-C socket
  ("USB2.0 host", VBUS always on). Port 0 (the power/OTG socket) is wired
  as a device-only port, see the mainline device tree.

  Sequence (from Linux phy-sun4i-usb.c, sun50i_h616_cfg):
    1. USB PHY1 clock + reset, OHCI1 12 MHz clock
    2. H616 quirk: PHY1/PHY3 only work if PHY2's SIDDQ is cleared as well
       (needs_phy2_siddq); PMU2 is reached through the EHCI2 bus clock
    3. Clear SIDDQ in PMU1, enable AHB "passby" (ULPI bypass, bursts)
    4. EHCI1/OHCI1 bus clocks + resets
    5. Register EHCI1 as a non-discoverable EHCI controller so that
       EhciDxe / UsbBusDxe / UsbKbDxe / UsbMassStorageDxe bind to it

  Low/full-speed devices (most keyboards and mice) plugged straight into
  the port are handed from EHCI1 to its companion OHCI1 (H616Pkg OhciDxe);
  behind a USB 2.0 hub they stay on EHCI (transaction translator).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/TimerLib.h>

#include <H616.h>

#define CCU_USB_CLK(n)       (H616_CCU_BASE + 0xA70 + (n) * 4)
#define   USB_CLK_OHCI_12M   BIT31
#define   USB_RST_PHY        BIT30
#define   USB_CLK_PHY        BIT29
#define CCU_USB_BGR          (H616_CCU_BASE + 0xA8C)
#define   BGR_OHCI(n)        (1U << (n))
#define   BGR_EHCI(n)        (1U << (4 + (n)))
#define   BGR_RST_OHCI(n)    (1U << (16 + (n)))
#define   BGR_RST_EHCI(n)    (1U << (20 + (n)))

// EHCI/OHCI/PMU bases for USB ports 0..3
STATIC CONST UINTN  mEhciBase[4] = { 0x05101000, 0x05200000, 0x05310000, 0x05311000 };
#define PMU_BASE(n)          (mEhciBase[n] + 0x800)
#define   PMU_PASSBY_BITS    (BIT10 | BIT9 | BIT8 | BIT0)
#define   PMU_HCI_PHY_CTL    0x10
#define   PMU_PHY_CTL_SIDDQ  BIT3


#define USB_PORT             1

/**
  Put one USB PHY and its EHCI/OHCI pair into reset with all clocks gated.
**/
STATIC
VOID
UsbPortShutdown (
  IN UINTN  Port
  )
{
  MmioAnd32 (CCU_USB_BGR, ~(UINT32)(BGR_RST_OHCI (Port) | BGR_RST_EHCI (Port)));
  MicroSecondDelay (10);
  MmioAnd32 (CCU_USB_BGR, ~(UINT32)(BGR_OHCI (Port) | BGR_EHCI (Port)));
  MmioAnd32 (CCU_USB_CLK (Port), ~(UINT32)USB_RST_PHY);
  MicroSecondDelay (10);
  MmioAnd32 (CCU_USB_CLK (Port), ~(UINT32)(USB_CLK_PHY | USB_CLK_OHCI_12M));
  MicroSecondDelay (100);
}

/**
  Bring a USB PHY and its EHCI/OHCI pair out of reset.  Clocks are always
  started before the matching reset is released (as Linux does); releasing
  both in the same register write can leave the PHY in a bad state.
**/
STATIC
VOID
UsbPhyPowerUp (
  IN UINTN  Port
  )
{
  // PHY clock (+ OHCI 12 MHz clock) first, then release the PHY reset
  MmioOr32 (CCU_USB_CLK (Port), USB_CLK_PHY | USB_CLK_OHCI_12M);
  MicroSecondDelay (20);
  MmioOr32 (CCU_USB_CLK (Port), USB_RST_PHY);
  MicroSecondDelay (100);

  // Controller bus clocks first, then release the controller resets
  MmioOr32 (CCU_USB_BGR, BGR_OHCI (Port) | BGR_EHCI (Port));
  MicroSecondDelay (20);
  MmioOr32 (CCU_USB_BGR, BGR_RST_OHCI (Port) | BGR_RST_EHCI (Port));
  MicroSecondDelay (100);
}

STATIC
VOID
UsbBringUp (
  VOID
  )
{
  UsbPortShutdown (USB_PORT);
  UsbPortShutdown (2);

  // H616 quirk: PHY2 must be out of SIDDQ for PHY1/PHY3 to work.
  UsbPhyPowerUp (2);
  MmioAnd32 (PMU_BASE (2) + PMU_HCI_PHY_CTL, ~(UINT32)PMU_PHY_CTL_SIDDQ);

  UsbPhyPowerUp (USB_PORT);
  MmioAnd32 (PMU_BASE (USB_PORT) + PMU_HCI_PHY_CTL, ~(UINT32)PMU_PHY_CTL_SIDDQ);
  MmioOr32 (PMU_BASE (USB_PORT), PMU_PASSBY_BITS);
  MicroSecondDelay (1000);
}

EFI_STATUS
EFIAPI
SunxiUsbDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  UsbBringUp ();

  DEBUG ((
    DEBUG_INFO,
    "SunxiUsb: port %u up, EHCI @ 0x%lx (CCU USB%u=0x%08x BGR=0x%08x PMU=0x%08x/0x%08x)\n",
    USB_PORT,
    (UINT64)mEhciBase[USB_PORT],
    USB_PORT,
    MmioRead32 (CCU_USB_CLK (USB_PORT)),
    MmioRead32 (CCU_USB_BGR),
    MmioRead32 (PMU_BASE (USB_PORT)),
    MmioRead32 (PMU_BASE (USB_PORT) + PMU_HCI_PHY_CTL)
    ));

  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeEhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             NULL,
             NULL,
             1,
             mEhciBase[USB_PORT],
             (UINTN)SIZE_1KB
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SunxiUsb: EHCI registration failed: %r\n", Status));
  }

  //
  // OHCI1 (EHCI base + 0x400) is the companion controller: EHCI hands
  // low/full-speed devices plugged straight into the port (keyboards,
  // mice) over to it. Registered after the EHCI so that it is connected
  // second.
  //
  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeOhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             NULL,
             NULL,
             1,
             mEhciBase[USB_PORT] + 0x400,
             (UINTN)SIZE_1KB
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SunxiUsb: OHCI registration failed: %r\n", Status));
  }

  return Status;
}
