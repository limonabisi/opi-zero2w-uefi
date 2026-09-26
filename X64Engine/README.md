# x64 Engine

An x86-64 PC emulated by the firmware, so 64-bit x86 operating systems can run
on the Orange Pi Zero 2W (work in progress).

- CPU: unicorn-for-efi (QEMU TCG) in a new *system mode*
  (`patches/unicorn-0002-x64-engine-system-mode.patch`): real page tables,
  CPU exceptions and SYSCALL delivered to the guest, external interrupts,
  exits only at block boundaries.
- Devices: 8259 PIC, 8254 PIT, 16550 UART, MC146818 RTC, i8042 keyboard.
- Boot: Linux boot protocol (bzImage + initrd).

Status: an Ubuntu x86-64 kernel boots to a BusyBox shell on a development PC
(`make && ./x64e bzImage initrd.cpio.gz`). Framebuffer, disk, ISO boot and the
firmware (UEFI) host are next.
