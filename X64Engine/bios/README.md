# BIOS images of the "PC with a BIOS" machine

`bios.bin` is [SeaBIOS](https://www.seabios.org/) rel-1.16.3 and `vgabios.bin` its
SeaVGABIOS (Bochs VBE flavour), both LGPL-3.0. They are built from the unmodified
release plus `patches/seabios-0001-vga-modes-fit-the-screen.patch` (the video BIOS
only offers modes that fit the screen's height as well as its width):

```sh
git clone --depth 1 -b rel-1.16.3 https://github.com/coreboot/seabios && cd seabios
git apply ../patches/seabios-0001-vga-modes-fit-the-screen.patch
cp ../X64Engine/bios/seabios.config .config && make olddefconfig && make      # out/bios.bin
make KCONFIG_CONFIG=../X64Engine/bios/vgabios.config OUT=out-vga/ olddefconfig
make KCONFIG_CONFIG=../X64Engine/bios/vgabios.config OUT=out-vga/ out-vga/vgabios.bin
```

`seabios.config`: QEMU machine type, 256 KB ROM, debug messages to port 0x402
(the engine sends them to the board's UART), no SMM. `scripts/build-x64engine.sh`
turns the two images into `bios_blob.c`, which is linked into `X64Engine.efi`.
