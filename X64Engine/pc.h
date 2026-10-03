/*
 * x64 Engine - the "PC with a BIOS" machine: what Windows and other systems
 * that start from a BIOS need. It looks like QEMU's i440FX PC:
 *
 *   00:00.0 8086:1237 i440FX host bridge      00:01.0 8086:7000 PIIX3 ISA
 *   00:01.1 8086:7010 PIIX3 IDE               00:01.3 8086:7113 PIIX4 PM (ACPI)
 *   00:02.0 1234:1111 VGA with Bochs VBE
 *   PIC, PIT, RTC, i8042 keyboard + mouse, COM1, fw_cfg (0x510),
 *   local APIC (0xFEE00000), IO-APIC (0xFEC00000)
 *
 * SeaBIOS is the BIOS; it brings its own ACPI tables for this chipset.
 */
#ifndef X64E_PC_H
#define X64E_PC_H

#include "x64e.h"

#define PM_BASE      0xb000        /* SeaBIOS puts the PIIX4 PM block here */

/* the BIOS images (bios_blob.c, generated from X64Engine/bios/ by the build) */
extern const unsigned char x64e_seabios[], x64e_vgabios[];
extern const unsigned int  x64e_seabios_size, x64e_vgabios_size;

/* pc.c */
int      pc_setup (machine_t *m, const uint8_t *bios, size_t bios_size,
                   const uint8_t *vgabios, size_t vgabios_size);
void     pc_reset (machine_t *m);
int      pc_io_read (machine_t *m, uint16_t port, int size, uint32_t *val);
int      pc_io_write (machine_t *m, uint16_t port, int size, uint32_t val);
void     pc_tick (machine_t *m, uint64_t now);
uint64_t pc_next_event (machine_t *m);
void     pci_set_intx (machine_t *m, pci_dev_t *d, int level);
uint8_t *machine_ram (machine_t *m, uint64_t gpa, uint64_t len);   /* NULL if not RAM */
void     cpu_irq_update (machine_t *m);       /* PIC or APIC state changed */

/* ide.c: PIIX3 IDE, ATA disks and ATAPI CD-ROMs */
void     ide_init (machine_t *m);
void     ide_reset (machine_t *m);
int      ide_attach (machine_t *m, int channel, int unit, void *disk, uint64_t size_bytes,
                     int cdrom, int readonly);
int      ide_io_read (machine_t *m, uint16_t port, int size, uint32_t *val);
int      ide_io_write (machine_t *m, uint16_t port, int size, uint32_t val);

/* vga.c: VGA + Bochs VBE, drawn into the host frame buffer */
void     vga_init (machine_t *m);
void     vga_reset (machine_t *m);
int      vga_io_read (machine_t *m, uint16_t port, int size, uint32_t *val);
int      vga_io_write (machine_t *m, uint16_t port, int size, uint32_t val);
void     vga_update (machine_t *m);           /* redraw the host screen */

/* apic.c: local APIC and IO-APIC */
void     apic_init (machine_t *m);
void     apic_reset (machine_t *m);
void     ioapic_set_irq (machine_t *m, int pin, int level);
int      apic_has_interrupt (machine_t *m);   /* a vector the CPU would take now */
int      apic_ack_interrupt (machine_t *m);   /* take it: returns the vector */
int      apic_accepts_pic (machine_t *m);     /* 8259 INT reaches the CPU */
void     apic_tick (machine_t *m, uint64_t now);
uint64_t apic_next_event (machine_t *m);

/* rtc.c: MC146818 clock and CMOS memory */
void     rtc_init (machine_t *m);
int      rtc_io_read (machine_t *m, uint16_t port, int size, uint32_t *val);
int      rtc_io_write (machine_t *m, uint16_t port, int size, uint32_t val);
void     rtc_tick (machine_t *m, uint64_t now);
uint64_t rtc_next_event (machine_t *m);
void     rtc_set_cmos (machine_t *m, unsigned index, uint8_t val);

#endif
