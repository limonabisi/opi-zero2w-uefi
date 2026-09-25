/** @file
  SerialPortLib for the Allwinner DesignWare APB UART (UART0).

  U-Boot SPL has already configured UART0 (pins PH0/PH1, clock, 115200 8N1)
  and TF-A BL31 printed through it. This library therefore does NOT
  reprogram the divisor or line control: on the DW UART, LCR writes are
  silently dropped while the UART is busy (for example when noise is
  arriving on RX), which could leave the port in a half-configured state.
  It only polls LSR and reads/writes the data register.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/SerialPortLib.h>
#include <Protocol/SerialIo.h>

#define UART_RBR   0x00
#define UART_THR   0x00
#define UART_FCR   0x08
#define UART_LSR   0x14
#define UART_MSR   0x18
#define LSR_DR     BIT0
#define LSR_THRE   BIT5
#define LSR_TEMT   BIT6

#define UART_BASE  ((UINTN)FixedPcdGet64 (PcdSerialRegisterBase))

RETURN_STATUS
EFIAPI
SerialPortInitialize (
  VOID
  )
{
  return RETURN_SUCCESS;
}

UINTN
EFIAPI
SerialPortWrite (
  IN UINT8  *Buffer,
  IN UINTN  NumberOfBytes
  )
{
  UINTN  Index;
  UINTN  Spin;

  if (Buffer == NULL) {
    return 0;
  }

  for (Index = 0; Index < NumberOfBytes; Index++) {
    // Bounded wait so a stuck UART can never hang the firmware.
    for (Spin = 0; Spin < 1000000; Spin++) {
      if ((MmioRead32 (UART_BASE + UART_LSR) & LSR_THRE) != 0) {
        break;
      }
    }

    MmioWrite32 (UART_BASE + UART_THR, Buffer[Index]);
  }

  return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortRead (
  OUT UINT8  *Buffer,
  IN  UINTN  NumberOfBytes
  )
{
  UINTN  Index;

  if ((Buffer == NULL) || !FeaturePcdGet (PcdSunxiUartInputEnable)) {
    return 0;
  }

  for (Index = 0; Index < NumberOfBytes; Index++) {
    while ((MmioRead32 (UART_BASE + UART_LSR) & LSR_DR) == 0) {
    }

    Buffer[Index] = (UINT8)MmioRead32 (UART_BASE + UART_RBR);
  }

  return NumberOfBytes;
}

BOOLEAN
EFIAPI
SerialPortPoll (
  VOID
  )
{
  if (!FeaturePcdGet (PcdSunxiUartInputEnable)) {
    return FALSE;
  }

  return (MmioRead32 (UART_BASE + UART_LSR) & LSR_DR) != 0;
}

RETURN_STATUS
EFIAPI
SerialPortSetControl (
  IN UINT32  Control
  )
{
  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
SerialPortGetControl (
  OUT UINT32  *Control
  )
{
  UINT32  Lsr;

  Lsr      = MmioRead32 (UART_BASE + UART_LSR);
  *Control = EFI_SERIAL_CLEAR_TO_SEND | EFI_SERIAL_DATA_SET_READY | EFI_SERIAL_CARRIER_DETECT;
  if (((Lsr & LSR_DR) == 0) || !FeaturePcdGet (PcdSunxiUartInputEnable)) {
    *Control |= EFI_SERIAL_INPUT_BUFFER_EMPTY;
  }

  if ((Lsr & LSR_TEMT) != 0) {
    *Control |= EFI_SERIAL_OUTPUT_BUFFER_EMPTY;
  }

  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
SerialPortSetAttributes (
  IN OUT UINT64              *BaudRate,
  IN OUT UINT32              *ReceiveFifoDepth,
  IN OUT UINT32              *Timeout,
  IN OUT EFI_PARITY_TYPE     *Parity,
  IN OUT UINT8               *DataBits,
  IN OUT EFI_STOP_BITS_TYPE  *StopBits
  )
{
  // Fixed configuration inherited from U-Boot SPL.
  *BaudRate         = FixedPcdGet32 (PcdSerialBaudRate);
  *ReceiveFifoDepth = 64;
  if (*Timeout == 0) {
    *Timeout = 1000000;
  }

  *Parity   = NoParity;
  *DataBits = 8;
  *StopBits = OneStopBit;
  return RETURN_SUCCESS;
}
