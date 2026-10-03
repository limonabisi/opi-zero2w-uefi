# x64 Engine

An x86-64 PC emulated by the firmware, so 64-bit x86 operating systems can run
on the Orange Pi Zero 2W.

## Parts

- **CPU**: unicorn-for-efi (QEMU TCG) in a new *system mode*
  (`patches/unicorn-0002-x64-engine-system-mode.patch`): real page tables,
  CPU exceptions and SYSCALL delivered to the guest, external interrupts,
  exits only at block boundaries.
- **Devices** (`machine.c`, `pic.c`, `pit.c`, `uart.c`, `i8042.c`, `pci.c`,
  `virtio_blk.c`, `virtio_net.c`): 8259 PIC, 8254 PIT, 16550 UART, MC146818
  RTC, i8042 keyboard, PCI (config mechanism 1), virtio-blk and virtio-net
  (legacy PCI), a linear framebuffer.
- **Boot** (`bootcfg.c`, `linuxboot.c`, `iso9660.c`, `ext4.c`, `unpack.c`):
  Linux boot protocol (bzImage + initrd) straight from an ISO or from an
  installed system: ISO9660 / Rock Ridge and ext2/3/4 readers, GRUB `grub.cfg`
  and isolinux menu parsers. The initrd is unpacked natively (gzip, LZ4).
- **Hosts**: `host_uefi.c` (the firmware: GOP framebuffer, USB keyboard,
  BlockIo / files / exFAT for disks, SNP for the network card, `x64e_el.c`
  for the drop to EL1) and `host_posix.c` (a development PC, `make`).

## Speed

All of this is in the unicorn patch unless noted.

| | |
|---|---|
| Shadow MMU | The engine runs at EL1 with the firmware's identity map in TTBR0. TTBR1 holds shadow page tables for the x86 address space (`host VA = guest VA \| 0xffff000000000000`), filled on data aborts from the guest's page tables. Translated code then reads and writes guest memory with one instruction; user-mode blocks use LDTR / STTR so the MMU enforces the x86 U/S bit. Pages with translated code are mapped read-only. |
| Address spaces | One tree per PCID slot (7); a CR3 write with NOFLUSH just switches TTBR1. The upper half (global kernel pages) is one tree shared by all slots, so a new process does not fault the kernel in again. |
| Reverse map | Per RAM page, up to three writable shadow PTEs, to take write access away when the page gets code. No scanning, stale references are harmless. |
| Chaining | Blocks in the upper half are chained directly across pages. |
| Inline lookup | RET, indirect jumps and jumps to another page probe the jump cache in the translated code itself (branch-free, about 30 instructions) and call the helper only on a miss. |
| Bulk string operations | `rep movs` / `rep stos` with DF clear are done by `memmove` / `memset` on host pointers, a page pair at a time, with RSI / RDI / RCX kept exact. ERMS is advertised so kernels and C libraries use them. |
| Initrd | Unpacked by the engine before the kernel starts (`unpack.c`); the x86 kernel would spend most of a minute on it. |
| ASLR | `norandmaps` on the kernel command line: with randomization every process runs the same libraries at new addresses and every block is translated again. |

`X64E.CFG` keys (defaults in brackets): `shadow` [1], `el1` [1], `kchain` [1],
`lookup` [1], `bulk` [1], `unpack` [1], `aslr` [0], `serial` [0], `stats` [0].

## Measuring

**F11** (or `stats=1`) draws the counters on the screen: shadow fills and slow
paths, time spent on MMU faults and on translation, code cache use, free
firmware memory. The same lines go to the UART every 30 s.

The QEMU test build (`-D QEMU_TEST`) runs the engine on `qemu-system-aarch64
-M virt,virtualization=on`; with `-icount shift=0` the guest's clock counts
host instructions, which makes changes comparable to a fraction of a percent.

## Development PC

```sh
cd X64Engine && make            # needs a libunicorn.a built from src/unicorn
./x64e bzImage initrd.gz "console=ttyS0"
X64E_ISO=mini.iso ./x64e
```
