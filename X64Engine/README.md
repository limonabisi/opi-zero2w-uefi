# x64 Engine

An x86-64 PC emulated by the firmware, so 64-bit x86 operating systems can run
on the Orange Pi Zero 2W (work in progress).

- CPU: unicorn-for-efi (QEMU TCG) in a new *system mode*
  (`patches/unicorn-0002-x64-engine-system-mode.patch`): real page tables,
  CPU exceptions and SYSCALL delivered to the guest, external interrupts,
  exits only at block boundaries.
- Devices: 8259 PIC, 8254 PIT, 16550 UART, MC146818 RTC, i8042 keyboard,
  PCI (config mechanism 1), virtio-blk (legacy PCI).
- Boot: Linux boot protocol (bzImage + initrd), or straight from an ISO:
  ISO9660/Rock Ridge reader, GRUB `grub.cfg` / isolinux menu parser; the ISO
  itself is attached read-only as `/dev/vda`.

Status (development PC): an Ubuntu x86-64 kernel boots to a shell, and the
Ubuntu 20.04 x86-64 `mini.iso` boots to its installer
(`X64E_ISO=mini.iso ./x64e`). Framebuffer and the firmware (UEFI) host are next.
