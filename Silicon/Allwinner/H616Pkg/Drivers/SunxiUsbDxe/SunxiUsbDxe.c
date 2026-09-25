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

  Low/full-speed devices (most keyboards!) are routed to the OHCI companion
  on a root port; EDK2 has no OHCI driver yet, so plug the keyboard in
  through a USB 2.0 hub (the hub's transaction translator lets EHCI talk to
  it).

  The bring-up is checked with a short self test (a connected hub must not
  sit in a K line state and a root port reset must complete) and redone up
  to four times: on some boots the PHY otherwise came up stuck.

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

//
// EHCI operational registers (CAPLENGTH is 0x10 on sunxi)
//
#define EHCI_OP(n)           (mEhciBase[n] + 0x10)
#define   EHCI_USBCMD        0x00
#define     USBCMD_RUN       BIT0
#define     USBCMD_HCRESET   BIT1
#define   EHCI_USBSTS        0x04
#define     USBSTS_HALTED    BIT12
#define   EHCI_CONFIGFLAG    0x40
#define   EHCI_PORTSC        0x44
#define     PORTSC_CCS       BIT0
#define     PORTSC_CSC       BIT1
#define     PORTSC_PE        BIT2
#define     PORTSC_PEC       BIT3
#define     PORTSC_OCC       BIT5
#define     PORTSC_PR        BIT8
#define     PORTSC_LS_MASK   (BIT11 | BIT10)
#define     PORTSC_LS_K      BIT10
#define     PORTSC_PP        BIT12
#define     PORTSC_CHANGE    (PORTSC_CSC | PORTSC_PEC | PORTSC_OCC)

#define MAX_BRINGUP_TRIES    4

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

STATIC
VOID
EhciHcReset (
  IN UINTN  Op
  )
{
  UINTN  Index;

  MmioAnd32 (Op + EHCI_USBCMD, ~(UINT32)USBCMD_RUN);
  for (Index = 0; Index < 100 && !(MmioRead32 (Op + EHCI_USBSTS) & USBSTS_HALTED); Index++) {
    MicroSecondDelay (100);
  }

  MmioWrite32 (Op + EHCI_USBCMD, USBCMD_HCRESET);
  for (Index = 0; Index < 100 && (MmioRead32 (Op + EHCI_USBCMD) & USBCMD_HCRESET); Index++) {
    MicroSecondDelay (100);
  }
}

/**
  Run the EHCI briefly and do one root port reset, to check that the PHY
  came up sane: a connected hub must not show a stuck K line state and the
  port reset must complete.  The controller is halted and reset afterwards
  so that EhciDxe starts from a clean state.

  @retval TRUE   Port looks healthy (or nothing is connected).
  @retval FALSE  PHY/port is stuck; the caller should redo the bring-up.
**/
STATIC
BOOLEAN
UsbPortSelfTest (
  IN UINTN  Try
  )
{
  UINTN    Op;
  UINT32   Sc;
  UINT32   ScReset;
  UINTN    Index;
  BOOLEAN  Ok;

  Op      = EHCI_OP (USB_PORT);
  Ok      = TRUE;
  ScReset = 0;

  EhciHcReset (Op);

  // Run with the port routed to EHCI and powered
  MmioWrite32 (Op + EHCI_USBCMD, USBCMD_RUN);
  MmioWrite32 (Op + EHCI_CONFIGFLAG, 1);
  Sc = MmioRead32 (Op + EHCI_PORTSC);
  MmioWrite32 (Op + EHCI_PORTSC, (Sc & ~PORTSC_CHANGE) | PORTSC_PP);

  // Give a hub up to 300 ms to connect and settle (USB debounce = 100 ms)
  for (Index = 0; Index < 30; Index++) {
    MicroSecondDelay (10000);
    Sc = MmioRead32 (Op + EHCI_PORTSC);
    if ((Index >= 10) && (Sc & PORTSC_CCS) && ((Sc & PORTSC_LS_MASK) != PORTSC_LS_K)) {
      break;
    }
  }

  if (Sc & PORTSC_CCS) {
    if ((Sc & PORTSC_LS_MASK) == PORTSC_LS_K) {
      // Hubs and HS/FS devices never idle in K: the PHY did not come up
      Ok = FALSE;
    } else {
      // 50 ms bus reset; the controller must finish it on its own
      MmioWrite32 (Op + EHCI_PORTSC, (Sc & ~(PORTSC_CHANGE | PORTSC_PE)) | PORTSC_PR);
      MicroSecondDelay (50000);
      MmioAnd32 (Op + EHCI_PORTSC, ~(UINT32)(PORTSC_CHANGE | PORTSC_PR));
      for (Index = 0; Index < 100; Index++) {
        MicroSecondDelay (1000);
        ScReset = MmioRead32 (Op + EHCI_PORTSC);
        if (!(ScReset & PORTSC_PR)) {
          break;
        }
      }

      if (ScReset & PORTSC_PR) {
        Ok = FALSE;
      }
    }
  }

  DEBUG ((
    Ok ? DEBUG_INFO : DEBUG_WARN,
    "SunxiUsb: bring-up %u: PORTSC=0x%08x%a, after reset 0x%08x -> %a\n",
    (UINT32)Try,
    Sc,
    (Sc & PORTSC_CCS) ? " (device)" : " (empty)",
    ScReset,
    Ok ? "ok" : "PHY stuck, retrying"
    ));

  EhciHcReset (Op);
  return Ok;
}

EFI_STATUS
EFIAPI
SunxiUsbDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINTN       Try;

  for (Try = 1; Try <= MAX_BRINGUP_TRIES; Try++) {
    UsbBringUp ();
    if (UsbPortSelfTest (Try)) {
      break;
    }
  }

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

  return Status;
}
