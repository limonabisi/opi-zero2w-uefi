/** @file
  The legacy EFI_USB_HC_PROTOCOL (USB 1.1 host controller) interface, used
  internally by this OHCI driver. It was dropped from MdePkg; the driver now
  publishes EFI_USB2_HC_PROTOCOL through thin wrappers (OhciUsb2.c).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef USB_HC_V1_H_
#define USB_HC_V1_H_

#include <Protocol/Usb2HostController.h>

typedef struct _EFI_USB_HC_PROTOCOL EFI_USB_HC_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_USB_HC_PROTOCOL_RESET)(
  IN EFI_USB_HC_PROTOCOL  *This,
  IN UINT16               Attributes
  );

typedef EFI_STATUS (EFIAPI *EFI_USB_HC_PROTOCOL_GET_STATE)(
  IN  EFI_USB_HC_PROTOCOL  *This,
  OUT EFI_USB_HC_STATE     *State
  );

typedef EFI_STATUS (EFIAPI *EFI_USB_HC_PROTOCOL_SET_STATE)(
  IN EFI_USB_HC_PROTOCOL  *This,
  IN EFI_USB_HC_STATE     State
  );

typedef EFI_STATUS (EFIAPI *EFI_USB_HC_PROTOCOL_CONTROL_TRANSFER)(
  IN     EFI_USB_HC_PROTOCOL     *This,
  IN     UINT8                   DeviceAddress,
  IN     BOOLEAN                 IsSlowDevice,
  IN     UINT8                   MaxPacketLength,
  IN     EFI_USB_DEVICE_REQUEST  *Request,
  IN     EFI_USB_DATA_DIRECTION  TransferDirection,
  IN OUT VOID                    *Data       OPTIONAL,
  IN OUT UINTN                   *DataLength OPTIONAL,
  IN     UINTN                   TimeOut,
  OUT    UINT32                  *TransferResult
  );

typedef EFI_STATUS (EFIAPI *EFI_USB_HC_PROTOCOL_BULK_TRANSFER)(
  IN     EFI_USB_HC_PROTOCOL  *This,
  IN     UINT8                DeviceAddress,
  IN     UINT8                EndPointAddress,
  IN     UINT8                MaxPacketLength,
  IN OUT VOID                 *Data,
  IN OUT UINTN                *DataLength,
  IN OUT UINT8                *DataToggle,
  IN     UINTN                TimeOut,
  OUT    UINT32               *TransferResult
  );

typedef EFI_STATUS (EFIAPI *EFI_USB_HC_PROTOCOL_ASYNC_INTERRUPT_TRANSFER)(
  IN     EFI_USB_HC_PROTOCOL              *This,
  IN     UINT8                            DeviceAddress,
  IN     UINT8                            EndPointAddress,
  IN     BOOLEAN                          IsSlowDevice,
  IN     UINT8                            MaxPacketLength,
  IN     BOOLEAN                          IsNewTransfer,
  IN OUT UINT8                            *DataToggle,
  IN     UINTN                            PollingInterval  OPTIONAL,
  IN     UINTN                            DataLength       OPTIONAL,
  IN     EFI_ASYNC_USB_TRANSFER_CALLBACK  CallBackFunction OPTIONAL,
  IN     VOID                             *Context         OPTIONAL
  );

typedef EFI_STATUS (EFIAPI *EFI_USB_HC_PROTOCOL_SYNC_INTERRUPT_TRANSFER)(
  IN     EFI_USB_HC_PROTOCOL  *This,
  IN     UINT8                DeviceAddress,
  IN     UINT8                EndPointAddress,
  IN     BOOLEAN              IsSlowDevice,
  IN     UINT8                MaxPacketLength,
  IN OUT VOID                 *Data,
  IN OUT UINTN                *DataLength,
  IN OUT UINT8                *DataToggle,
  IN     UINTN                TimeOut,
  OUT    UINT32               *TransferResult
  );

typedef EFI_STATUS (EFIAPI *EFI_USB_HC_PROTOCOL_ISOCHRONOUS_TRANSFER)(
  IN     EFI_USB_HC_PROTOCOL  *This,
  IN     UINT8                DeviceAddress,
  IN     UINT8                EndPointAddress,
  IN     UINT8                MaximumPacketLength,
  IN OUT VOID                 *Data,
  IN     UINTN                DataLength,
  OUT    UINT32               *TransferResult
  );

typedef EFI_STATUS (EFIAPI *EFI_USB_HC_PROTOCOL_ASYNC_ISOCHRONOUS_TRANSFER)(
  IN     EFI_USB_HC_PROTOCOL              *This,
  IN     UINT8                            DeviceAddress,
  IN     UINT8                            EndPointAddress,
  IN     UINT8                            MaximumPacketLength,
  IN OUT VOID                             *Data,
  IN     UINTN                            DataLength,
  IN     EFI_ASYNC_USB_TRANSFER_CALLBACK  IsochronousCallBack,
  IN     VOID                             *Context OPTIONAL
  );

typedef EFI_STATUS (EFIAPI *EFI_USB_HC_PROTOCOL_GET_ROOTHUB_PORT_NUMBER)(
  IN  EFI_USB_HC_PROTOCOL  *This,
  OUT UINT8                *PortNumber
  );

typedef EFI_STATUS (EFIAPI *EFI_USB_HC_PROTOCOL_GET_ROOTHUB_PORT_STATUS)(
  IN  EFI_USB_HC_PROTOCOL  *This,
  IN  UINT8                PortNumber,
  OUT EFI_USB_PORT_STATUS  *PortStatus
  );

typedef EFI_STATUS (EFIAPI *EFI_USB_HC_PROTOCOL_SET_ROOTHUB_PORT_FEATURE)(
  IN EFI_USB_HC_PROTOCOL   *This,
  IN UINT8                 PortNumber,
  IN EFI_USB_PORT_FEATURE  PortFeature
  );

typedef EFI_STATUS (EFIAPI *EFI_USB_HC_PROTOCOL_CLEAR_ROOTHUB_PORT_FEATURE)(
  IN EFI_USB_HC_PROTOCOL   *This,
  IN UINT8                 PortNumber,
  IN EFI_USB_PORT_FEATURE  PortFeature
  );

struct _EFI_USB_HC_PROTOCOL {
  EFI_USB_HC_PROTOCOL_RESET                          Reset;
  EFI_USB_HC_PROTOCOL_GET_STATE                      GetState;
  EFI_USB_HC_PROTOCOL_SET_STATE                      SetState;
  EFI_USB_HC_PROTOCOL_CONTROL_TRANSFER               ControlTransfer;
  EFI_USB_HC_PROTOCOL_BULK_TRANSFER                  BulkTransfer;
  EFI_USB_HC_PROTOCOL_ASYNC_INTERRUPT_TRANSFER       AsyncInterruptTransfer;
  EFI_USB_HC_PROTOCOL_SYNC_INTERRUPT_TRANSFER        SyncInterruptTransfer;
  EFI_USB_HC_PROTOCOL_ISOCHRONOUS_TRANSFER           IsochronousTransfer;
  EFI_USB_HC_PROTOCOL_ASYNC_ISOCHRONOUS_TRANSFER     AsyncIsochronousTransfer;
  EFI_USB_HC_PROTOCOL_GET_ROOTHUB_PORT_NUMBER        GetRootHubPortNumber;
  EFI_USB_HC_PROTOCOL_GET_ROOTHUB_PORT_STATUS        GetRootHubPortStatus;
  EFI_USB_HC_PROTOCOL_SET_ROOTHUB_PORT_FEATURE       SetRootHubPortFeature;
  EFI_USB_HC_PROTOCOL_CLEAR_ROOTHUB_PORT_FEATURE     ClearRootHubPortFeature;
  UINT16                                             MajorRevision;
  UINT16                                             MinorRevision;
};

#endif
