/** @file
  Common definitions for the Orange Pi Zero 2W (Allwinner H618) ACPI tables.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef OPI_ACPI_TABLES_H_
#define OPI_ACPI_TABLES_H_

#include <IndustryStandard/Acpi.h>

#define EFI_ACPI_OEM_ID            {'O','P','I','Z','2','W'}
#define EFI_ACPI_OEM_TABLE_ID      SIGNATURE_64 ('H','6','1','8','E','D','K','2')
#define EFI_ACPI_OEM_REVISION      0x00000100
#define EFI_ACPI_CREATOR_ID        SIGNATURE_32 ('E','D','K','2')
#define EFI_ACPI_CREATOR_REVISION  0x00000100

#define ACPI_HEADER(Signature, Type, Revision) {                  \
    Signature,                      /* UINT32  Signature */       \
    sizeof (Type),                  /* UINT32  Length */          \
    Revision,                       /* UINT8   Revision */        \
    0,                              /* UINT8   Checksum */        \
    EFI_ACPI_OEM_ID,                /* UINT8   OemId[6] */        \
    EFI_ACPI_OEM_TABLE_ID,          /* UINT64  OemTableId */      \
    EFI_ACPI_OEM_REVISION,          /* UINT32  OemRevision */     \
    EFI_ACPI_CREATOR_ID,            /* UINT32  CreatorId */       \
    EFI_ACPI_CREATOR_REVISION       /* UINT32  CreatorRevision */ \
  }

//
// H616 / H618 (see sun50i-h616.dtsi)
//
#define H616_GICD_BASE           0x03021000
#define H616_GICC_BASE           0x03022000
#define H616_GICH_BASE           0x03024000
#define H616_GICV_BASE           0x03026000
#define H616_GIC_MAINT_GSIV      25          // PPI 9
#define H616_PMU_GSIV(n)         (32 + 140 + (n))

#define H616_TIMER_SEC_GSIV      29          // PPI 13
#define H616_TIMER_NS_GSIV       30          // PPI 14
#define H616_TIMER_VIRT_GSIV     27          // PPI 11
#define H616_TIMER_HYP_GSIV      26          // PPI 10

#define H616_UART0_BASE          0x05000000
#define H616_UART0_GSIV          (32 + 0)
#define H616_EHCI1_BASE          0x05200000
#define H616_EHCI1_GSIV          (32 + 28)
#define H616_OHCI1_BASE          0x05200400
#define H616_OHCI1_GSIV          (32 + 29)

#endif
