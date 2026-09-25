/** @file
  DSDT for the Orange Pi Zero 2W (Allwinner H618).

  Only what the firmware has already brought up is described:
  the four CPUs and the USB 2.0 host port 1 (EHCI1 + OHCI1
  companion; PHY, clocks and resets are left running by SunxiUsbDxe).
  DMA is not cache coherent on this SoC (_CCA 0).

  UART0 is only described in SPCR/DBG2 (no namespace device), so that no
  Windows 16550 driver (8-bit register layout) grabs the 32-bit DW UART.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

DefinitionBlock ("Dsdt.aml", "DSDT", 2, "OPIZ2W", "H618EDK2", 0x00000100)
{
  Scope (\_SB)
  {
    Device (CPU0) { Name (_HID, "ACPI0007") Name (_UID, 0x0) Method (_STA) { Return (0xF) } }
    Device (CPU1) { Name (_HID, "ACPI0007") Name (_UID, 0x1) Method (_STA) { Return (0xF) } }
    Device (CPU2) { Name (_HID, "ACPI0007") Name (_UID, 0x2) Method (_STA) { Return (0xF) } }
    Device (CPU3) { Name (_HID, "ACPI0007") Name (_UID, 0x3) Method (_STA) { Return (0xF) } }

    //
    // USB port 1: EHCI1 (USB 2.0 high speed)
    //
    Device (USB0)
    {
      Name (_HID, "PNP0D20")           // EHCI
      Name (_UID, 0x0)
      Name (_CCA, 0x0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x05200000, 0x00000100)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 60 }
      })
    }

    //
    // USB port 1: OHCI1 (full/low speed companion)
    //
    Device (USB1)
    {
      Name (_HID, "PNP0D10")           // OHCI
      Name (_UID, 0x1)
      Name (_CCA, 0x0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x05200400, 0x00000100)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 61 }
      })
    }
  }
}
