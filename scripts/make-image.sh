#!/usr/bin/env bash
# Packs  U-Boot SPL + TF-A BL31 + EDK2 FD  into a sunxi boot image and an SD card image.
#
#   out/opi-zero2w-edk2-boot.bin : write raw to the SD card at offset 8 KiB
#   out/opi-zero2w-edk2-sd.img   : complete SD image (bootloader + FAT32 partition), flash with Etcher/Rufus
set -euo pipefail
TOP="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${SRC:-$TOP/src}"
TARGET="${1:-DEBUG}"
OUT="${OUT:-$TOP/out}"; mkdir -p "$OUT"
SPL="$SRC/u-boot/spl/sunxi-spl.bin"
BL31="$SRC/tf-a/build/sun50i_h616/release/bl31.bin"
FD="${FD:-$TOP/build/edk2-ws/Build/OrangePiZero2W/${TARGET}_GCC/FV/OPIZERO2W_EFI.fd}"
DTB="$TOP/Platform/OrangePi/OrangePiZero2W/DeviceTree/sun50i-h618-orangepi-zero2w.dtb"
for f in "$SPL" "$BL31" "$FD" "$DTB"; do [ -f "$f" ] || { echo "missing: $f"; exit 1; }; done

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
cp "$BL31" "$WORK/bl31.bin"; cp "$FD" "$WORK/edk2.fd"; cp "$DTB" "$WORK/board.dtb"

# FIT: SPL loads BL31 (firmware) and EDK2 (as the BL33 "U-Boot" loadable at 0x4A000000)
cat > "$WORK/edk2.its" <<ITS
/dts-v1/;
/ {
    description = "Orange Pi Zero 2W: TF-A BL31 + EDK2 UEFI";
    #address-cells = <1>;
    images {
        uboot {
            description = "EDK2 UEFI (BL33)";
            data = /incbin/("edk2.fd");
            type = "standalone";
            os = "u-boot";
            arch = "arm64";
            compression = "none";
            load = <0x4a000000>;
            entry = <0x4a000000>;
        };
        atf {
            description = "ARM Trusted Firmware BL31";
            data = /incbin/("bl31.bin");
            type = "firmware";
            os = "arm-trusted-firmware";
            arch = "arm64";
            compression = "none";
            load = <0x40000000>;
            entry = <0x40000000>;
        };
        fdt-1 {
            description = "sun50i-h618-orangepi-zero2w";
            data = /incbin/("board.dtb");
            type = "flat_dt";
            arch = "arm64";
            compression = "none";
        };
    };
    configurations {
        default = "config-1";
        config-1 {
            description = "sun50i-h618-orangepi-zero2w";
            firmware = "atf";
            loadables = "uboot";
            fdt = "fdt-1";
        };
    };
};
ITS
# -E: image data outside the FDT header, so SPL only reads the small header
# into its 1 MiB malloc area and copies each image straight to its load address.
( cd "$WORK" && mkimage -E -B 0x200 -f edk2.its edk2.itb >/dev/null )

# SPL (eGON header, 40 KiB) immediately followed by the FIT, exactly like u-boot-sunxi-with-spl.bin
BOOT="$OUT/opi-zero2w-edk2-boot.bin"
cat "$SPL" "$WORK/edk2.itb" > "$BOOT"
echo "boot image : $BOOT ($(stat -c %s "$BOOT") bytes)"

# Full SD image: 64 MiB, MBR, bootloader @ 8 KiB, FAT32 partition from 4 MiB
IMG="$OUT/opi-zero2w-edk2-sd.img"
rm -f "$IMG"; truncate -s 64M "$IMG"
printf 'label: dos\nstart=8192, type=c, bootable\n' | sfdisk -q "$IMG"
dd if="$BOOT" of="$IMG" bs=1024 seek=8 conv=notrunc status=none
PART="$WORK/part.img"; truncate -s $((60*1024*1024)) "$PART"
mkfs.vfat -F 32 -n OPIZ2WEFI "$PART" >/dev/null
mmd -i "$PART" ::/EFI ::/EFI/BOOT ::/dtb
mcopy -i "$PART" "$DTB" ::/dtb/sun50i-h618-orangepi-zero2w.dtb
# x64 Engine (x86-64 PC), started from the boot menu
X64E="$TOP/Platform/OrangePi/OrangePiZero2W/Binaries/X64Engine/X64Engine.efi"
if [ -f "$X64E" ]; then
  mmd -i "$PART" ::/EFI/X64ENGINE
  mcopy -i "$PART" "$X64E" ::/EFI/X64ENGINE/X64ENGINE.EFI
  # Engine settings: shadow=0 turns the shadow MMU off, el1=0 stays at EL2
  printf 'shadow=1\r\nel1=1\r\n' > "$WORK/X64E.CFG"
  mcopy -i "$PART" "$WORK/X64E.CFG" ::/EFI/X64ENGINE/X64E.CFG
fi
dd if="$PART" of="$IMG" bs=1M seek=4 conv=notrunc status=none
echo "SD image   : $IMG"
