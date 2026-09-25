#!/usr/bin/env bash
# One-shot build for Orange Pi Zero 2W EDK2 (Ubuntu 24.04 / WSL2 Ubuntu).
#   bash scripts/build-all.sh            # DEBUG firmware (verbose UART log)
#   bash scripts/build-all.sh RELEASE
set -euo pipefail
TOP="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="${1:-DEBUG}"
SRC="$TOP/src"; mkdir -p "$SRC"
J="$(nproc)"

UBOOT_TAG=v2025.07
TFA_TAG=v2.13.0
EDK2_TAG=edk2-stable202608

echo "==> Paketler"
sudo apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
  build-essential git python3 python3-setuptools python3-pyelftools python3-dev swig \
  gcc-aarch64-linux-gnu uuid-dev acpica-tools nasm bison flex bc libssl-dev libgnutls28-dev \
  device-tree-compiler u-boot-tools dosfstools mtools fdisk

echo "==> Kaynaklar"
[ -d "$SRC/u-boot" ] || git clone --depth 1 --branch $UBOOT_TAG https://github.com/u-boot/u-boot.git "$SRC/u-boot"
[ -d "$SRC/tf-a" ]   || git clone --depth 1 --branch $TFA_TAG https://github.com/ARM-software/arm-trusted-firmware.git "$SRC/tf-a"
if [ ! -d "$SRC/edk2" ]; then
  git clone --depth 1 --branch $EDK2_TAG https://github.com/tianocore/edk2.git "$SRC/edk2"
  git -C "$SRC/edk2" submodule update --init --depth 1
fi
make -C "$SRC/edk2/BaseTools" -j"$J"

echo "==> TF-A yamalari"
for p in "$TOP"/patches/tf-a-*.patch; do
  git -C "$SRC/tf-a" apply --check "$p" 2>/dev/null && git -C "$SRC/tf-a" apply "$p" && echo "  uygulandi: $(basename "$p")" || true
done

echo "==> TF-A BL31 (sun50i_h616)"
make -C "$SRC/tf-a" -j"$J" CROSS_COMPILE=aarch64-linux-gnu- PLAT=sun50i_h616 DEBUG=0 bl31

echo "==> U-Boot SPL (DRAM init, orangepi_zero2w_defconfig)"
make -C "$SRC/u-boot" orangepi_zero2w_defconfig
make -C "$SRC/u-boot" -j"$J" CROSS_COMPILE=aarch64-linux-gnu- \
  BL31="$SRC/tf-a/build/sun50i_h616/release/bl31.bin" SCP=/dev/null

echo "==> EDK2"
SRC="$SRC" bash "$TOP/scripts/build-edk2.sh" "$TARGET"

echo "==> SD imaji"
SRC="$SRC" bash "$TOP/scripts/make-image.sh" "$TARGET"
