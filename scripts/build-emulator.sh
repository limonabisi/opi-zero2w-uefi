#!/usr/bin/env bash
# Build the x64 UEFI emulator driver (intel/MultiArchUefiPkg + unicorn-for-efi)
# as a standalone EmulatorDxe.efi, bundled into the FD as a binary module.
set -euo pipefail
TOP="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${SRC:-$TOP/src}"
TARGET="${1:-RELEASE}"
MAU_REV=ea23ded14ba26a730077bd5638b7c461a7ac39c9
UNICORN_REV=40a8d07ba3cef067d9a6455c163dbed8d94f8e38
fetch() { [ -d "$2" ] || { git init -q "$2" && git -C "$2" fetch -q --depth 1 "$1" "$3" && git -C "$2" checkout -q FETCH_HEAD; }; }
fetch https://github.com/intel/MultiArchUefiPkg "$SRC/MultiArchUefiPkg" $MAU_REV
fetch https://github.com/intel/unicorn-for-efi.git "$SRC/unicorn" $UNICORN_REV
# Local fixes: current edk2 library layout, GRUB modules outside PE images,
# PIT port 0x61 for TSC calibration, CPUID FPU/TSC bits.
apply() { (cd "$1" && if git apply --check "$2" 2>/dev/null; then git apply "$2"; fi) }
apply "$SRC/MultiArchUefiPkg" "$TOP/patches/multiarchuefipkg-0001-opi-zero2w.patch"
apply "$SRC/unicorn" "$TOP/patches/unicorn-0001-cpuid-fpu-tsc.patch"
apply "$SRC/unicorn" "$TOP/patches/unicorn-0002-x64-engine-system-mode.patch"
export WORKSPACE="$TOP/build/emu-ws"
export PACKAGES_PATH="$SRC/edk2:$SRC:$TOP"
export GCC_AARCH64_PREFIX="${GCC_AARCH64_PREFIX:-aarch64-linux-gnu-}"
mkdir -p "$WORKSPACE"
set +u
source "$SRC/edk2/edksetup.sh" BaseTools >/dev/null
set -u
build -a AARCH64 -t GCC -b "$TARGET" -n "$(nproc)" -p MultiArchUefiPkg/Emulator.dsc -D MAU_EMU_X64_RAZ_WI_PIO=YES
OUT="$WORKSPACE/Build/MultiArchUefiPkg/${TARGET}_GCC/AARCH64/EmulatorDxe.efi"
mkdir -p "$TOP/Platform/OrangePi/OrangePiZero2W/Binaries/EmulatorDxe"
cp "$OUT" "$TOP/Platform/OrangePi/OrangePiZero2W/Binaries/EmulatorDxe/EmulatorDxe.efi"
ls -la "$TOP/Platform/OrangePi/OrangePiZero2W/Binaries/EmulatorDxe/EmulatorDxe.efi"

# x64 Engine (x86-64 PC), an application on the SD card's FAT partition
cp "$TOP/X64Engine/X64Engine.dsc" "$SRC/MultiArchUefiPkg/X64Engine.dsc"
build -a AARCH64 -t GCC -b "$TARGET" -n "$(nproc)" -p MultiArchUefiPkg/X64Engine.dsc
mkdir -p "$TOP/Platform/OrangePi/OrangePiZero2W/Binaries/X64Engine"
cp "$WORKSPACE/Build/X64Engine/${TARGET}_GCC/AARCH64/X64Engine.efi" "$TOP/Platform/OrangePi/OrangePiZero2W/Binaries/X64Engine/X64Engine.efi"
ls -la "$TOP/Platform/OrangePi/OrangePiZero2W/Binaries/X64Engine/X64Engine.efi"
