/** @file
  EFI_USB2_HC_PROTOCOL front end for the OHCI driver: every call is mapped
  onto the driver's (USB 1.1) EFI_USB_HC_PROTOCOL implementation.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Ohci.h"

STATIC
EFI_STATUS
EFIAPI
Ohci2GetCapability (
  IN  EFI_USB2_HC_PROTOCOL  *This,
  OUT UINT8                 *MaxSpeed,
  OUT UINT8                 *PortNumber,
  OUT UINT8                 *Is64BitCapable
  )
{
  USB_OHCI_HC_DEV  *Ohc;

  if ((MaxSpeed == NULL) || (PortNumber == NULL) || (Is64BitCapable == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Ohc             = USB_OHCI_HC_DEV_FROM_USB2_THIS (This);
  *MaxSpeed       = EFI_USB_SPEED_FULL;
  *Is64BitCapable = 0;
  return Ohc->UsbHc.GetRootHubPortNumber (&Ohc->UsbHc, PortNumber);
}

STATIC
EFI_STATUS
EFIAPI
Ohci2Reset (
  IN EFI_USB2_HC_PROTOCOL  *This,
  IN UINT16                Attributes
  )
{
  USB_OHCI_HC_DEV  *Ohc;

  Ohc = USB_OHCI_HC_DEV_FROM_USB2_THIS (This);
  if ((Attributes & (EFI_USB_HC_RESET_GLOBAL_WITH_DEBUG | EFI_USB_HC_RESET_HOST_WITH_DEBUG)) != 0) {
    return EFI_UNSUPPORTED;
  }

  return Ohc->UsbHc.Reset (&Ohc->UsbHc, Attributes);
}

STATIC
EFI_STATUS
EFIAPI
Ohci2GetState (
  IN  EFI_USB2_HC_PROTOCOL  *This,
  OUT EFI_USB_HC_STATE      *State
  )
{
  USB_OHCI_HC_DEV  *Ohc;

  Ohc = USB_OHCI_HC_DEV_FROM_USB2_THIS (This);
  return Ohc->UsbHc.GetState (&Ohc->UsbHc, State);
}

STATIC
EFI_STATUS
EFIAPI
Ohci2SetState (
  IN EFI_USB2_HC_PROTOCOL  *This,
  IN EFI_USB_HC_STATE      State
  )
{
  USB_OHCI_HC_DEV  *Ohc;

  Ohc = USB_OHCI_HC_DEV_FROM_USB2_THIS (This);
  return Ohc->UsbHc.SetState (&Ohc->UsbHc, State);
}

STATIC
EFI_STATUS
EFIAPI
Ohci2ControlTransfer (
  IN     EFI_USB2_HC_PROTOCOL                *This,
  IN     UINT8                               DeviceAddress,
  IN     UINT8                               DeviceSpeed,
  IN     UINTN                               MaximumPacketLength,
  IN     EFI_USB_DEVICE_REQUEST              *Request,
  IN     EFI_USB_DATA_DIRECTION              TransferDirection,
  IN OUT VOID                                *Data       OPTIONAL,
  IN OUT UINTN                               *DataLength OPTIONAL,
  IN     UINTN                               TimeOut,
  IN     EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
  OUT    UINT32                              *TransferResult
  )
{
  USB_OHCI_HC_DEV  *Ohc;

  if ((DeviceSpeed == EFI_USB_SPEED_HIGH) || (DeviceSpeed == EFI_USB_SPEED_SUPER) ||
      (MaximumPacketLength > 64))
  {
    return EFI_INVALID_PARAMETER;
  }

  Ohc = USB_OHCI_HC_DEV_FROM_USB2_THIS (This);
  return Ohc->UsbHc.ControlTransfer (
                      &Ohc->UsbHc,
                      DeviceAddress,
                      (BOOLEAN)(DeviceSpeed == EFI_USB_SPEED_LOW),
                      (UINT8)MaximumPacketLength,
                      Request,
                      TransferDirection,
                      Data,
                      DataLength,
                      TimeOut,
                      TransferResult
                      );
}

STATIC
EFI_STATUS
EFIAPI
Ohci2BulkTransfer (
  IN     EFI_USB2_HC_PROTOCOL                *This,
  IN     UINT8                               DeviceAddress,
  IN     UINT8                               EndPointAddress,
  IN     UINT8                               DeviceSpeed,
  IN     UINTN                               MaximumPacketLength,
  IN     UINT8                               DataBuffersNumber,
  IN OUT VOID                                *Data[EFI_USB_MAX_BULK_BUFFER_NUM],
  IN OUT UINTN                               *DataLength,
  IN OUT UINT8                               *DataToggle,
  IN     UINTN                               TimeOut,
  IN     EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
  OUT    UINT32                              *TransferResult
  )
{
  USB_OHCI_HC_DEV  *Ohc;

  if ((DeviceSpeed != EFI_USB_SPEED_FULL) || (MaximumPacketLength > 64) ||
      (Data == NULL) || (DataBuffersNumber == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  Ohc = USB_OHCI_HC_DEV_FROM_USB2_THIS (This);
  return Ohc->UsbHc.BulkTransfer (
                      &Ohc->UsbHc,
                      DeviceAddress,
                      EndPointAddress,
                      (UINT8)MaximumPacketLength,
                      Data[0],
                      DataLength,
                      DataToggle,
                      TimeOut,
                      TransferResult
                      );
}

STATIC
EFI_STATUS
EFIAPI
Ohci2AsyncInterruptTransfer (
  IN     EFI_USB2_HC_PROTOCOL                *This,
  IN     UINT8                               DeviceAddress,
  IN     UINT8                               EndPointAddress,
  IN     UINT8                               DeviceSpeed,
  IN     UINTN                               MaximumPacketLength,
  IN     BOOLEAN                             IsNewTransfer,
  IN OUT UINT8                               *DataToggle,
  IN     UINTN                               PollingInterval  OPTIONAL,
  IN     UINTN                               DataLength       OPTIONAL,
  IN     EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator      OPTIONAL,
  IN     EFI_ASYNC_USB_TRANSFER_CALLBACK     CallBackFunction OPTIONAL,
  IN     VOID                                *Context         OPTIONAL
  )
{
  USB_OHCI_HC_DEV  *Ohc;

  if (IsNewTransfer && (MaximumPacketLength > 64)) {
    return EFI_INVALID_PARAMETER;
  }

  Ohc = USB_OHCI_HC_DEV_FROM_USB2_THIS (This);
  return Ohc->UsbHc.AsyncInterruptTransfer (
                      &Ohc->UsbHc,
                      DeviceAddress,
                      EndPointAddress,
                      (BOOLEAN)(DeviceSpeed == EFI_USB_SPEED_LOW),
                      (UINT8)MaximumPacketLength,
                      IsNewTransfer,
                      DataToggle,
                      PollingInterval,
                      DataLength,
                      CallBackFunction,
                      Context
                      );
}

STATIC
EFI_STATUS
EFIAPI
Ohci2SyncInterruptTransfer (
  IN     EFI_USB2_HC_PROTOCOL                *This,
  IN     UINT8                               DeviceAddress,
  IN     UINT8                               EndPointAddress,
  IN     UINT8                               DeviceSpeed,
  IN     UINTN                               MaximumPacketLength,
  IN OUT VOID                                *Data,
  IN OUT UINTN                               *DataLength,
  IN OUT UINT8                               *DataToggle,
  IN     UINTN                               TimeOut,
  IN     EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
  OUT    UINT32                              *TransferResult
  )
{
  USB_OHCI_HC_DEV  *Ohc;

  if (MaximumPacketLength > 64) {
    return EFI_INVALID_PARAMETER;
  }

  Ohc = USB_OHCI_HC_DEV_FROM_USB2_THIS (This);
  return Ohc->UsbHc.SyncInterruptTransfer (
                      &Ohc->UsbHc,
                      DeviceAddress,
                      EndPointAddress,
                      (BOOLEAN)(DeviceSpeed == EFI_USB_SPEED_LOW),
                      (UINT8)MaximumPacketLength,
                      Data,
                      DataLength,
                      DataToggle,
                      TimeOut,
                      TransferResult
                      );
}

STATIC
EFI_STATUS
EFIAPI
Ohci2IsochronousTransfer (
  IN     EFI_USB2_HC_PROTOCOL                *This,
  IN     UINT8                               DeviceAddress,
  IN     UINT8                               EndPointAddress,
  IN     UINT8                               DeviceSpeed,
  IN     UINTN                               MaximumPacketLength,
  IN     UINT8                               DataBuffersNumber,
  IN OUT VOID                                *Data[EFI_USB_MAX_ISO_BUFFER_NUM],
  IN     UINTN                               DataLength,
  IN     EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
  OUT    UINT32                              *TransferResult
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
Ohci2AsyncIsochronousTransfer (
  IN     EFI_USB2_HC_PROTOCOL                *This,
  IN     UINT8                               DeviceAddress,
  IN     UINT8                               EndPointAddress,
  IN     UINT8                               DeviceSpeed,
  IN     UINTN                               MaximumPacketLength,
  IN     UINT8                               DataBuffersNumber,
  IN OUT VOID                                *Data[EFI_USB_MAX_ISO_BUFFER_NUM],
  IN     UINTN                               DataLength,
  IN     EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
  IN     EFI_ASYNC_USB_TRANSFER_CALLBACK     IsochronousCallBack,
  IN     VOID                                *Context
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
Ohci2GetRootHubPortStatus (
  IN  EFI_USB2_HC_PROTOCOL  *This,
  IN  UINT8                 PortNumber,
  OUT EFI_USB_PORT_STATUS   *PortStatus
  )
{
  USB_OHCI_HC_DEV  *Ohc;

  Ohc = USB_OHCI_HC_DEV_FROM_USB2_THIS (This);
  return Ohc->UsbHc.GetRootHubPortStatus (&Ohc->UsbHc, PortNumber, PortStatus);
}

STATIC
EFI_STATUS
EFIAPI
Ohci2SetRootHubPortFeature (
  IN EFI_USB2_HC_PROTOCOL  *This,
  IN UINT8                 PortNumber,
  IN EFI_USB_PORT_FEATURE  PortFeature
  )
{
  USB_OHCI_HC_DEV  *Ohc;

  Ohc = USB_OHCI_HC_DEV_FROM_USB2_THIS (This);
  return Ohc->UsbHc.SetRootHubPortFeature (&Ohc->UsbHc, PortNumber, PortFeature);
}

STATIC
EFI_STATUS
EFIAPI
Ohci2ClearRootHubPortFeature (
  IN EFI_USB2_HC_PROTOCOL  *This,
  IN UINT8                 PortNumber,
  IN EFI_USB_PORT_FEATURE  PortFeature
  )
{
  USB_OHCI_HC_DEV  *Ohc;

  Ohc = USB_OHCI_HC_DEV_FROM_USB2_THIS (This);
  return Ohc->UsbHc.ClearRootHubPortFeature (&Ohc->UsbHc, PortNumber, PortFeature);
}

VOID
OhciInitUsb2Protocol (
  IN USB_OHCI_HC_DEV  *Ohc
  )
{
  Ohc->Usb2Hc.GetCapability            = Ohci2GetCapability;
  Ohc->Usb2Hc.Reset                    = Ohci2Reset;
  Ohc->Usb2Hc.GetState                 = Ohci2GetState;
  Ohc->Usb2Hc.SetState                 = Ohci2SetState;
  Ohc->Usb2Hc.ControlTransfer          = Ohci2ControlTransfer;
  Ohc->Usb2Hc.BulkTransfer             = Ohci2BulkTransfer;
  Ohc->Usb2Hc.AsyncInterruptTransfer   = Ohci2AsyncInterruptTransfer;
  Ohc->Usb2Hc.SyncInterruptTransfer    = Ohci2SyncInterruptTransfer;
  Ohc->Usb2Hc.IsochronousTransfer      = Ohci2IsochronousTransfer;
  Ohc->Usb2Hc.AsyncIsochronousTransfer = Ohci2AsyncIsochronousTransfer;
  Ohc->Usb2Hc.GetRootHubPortStatus     = Ohci2GetRootHubPortStatus;
  Ohc->Usb2Hc.SetRootHubPortFeature    = Ohci2SetRootHubPortFeature;
  Ohc->Usb2Hc.ClearRootHubPortFeature  = Ohci2ClearRootHubPortFeature;
  Ohc->Usb2Hc.MajorRevision            = 0x1;
  Ohc->Usb2Hc.MinorRevision            = 0x1;
}
