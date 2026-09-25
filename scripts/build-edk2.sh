#!/usr/bin/env bash
# Build the EDK2 firmware (FD) for Orange Pi Zero 2W.
# Usage: scripts/build-edk2.sh [DEBUG|RELEASE]
set -euo pipefail
TOP="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${SRC:-$TOP/src}"
TARGET="${1:-DEBUG}"
export WORKSPACE="$TOP/build/edk2-ws"
export PACKAGES_PATH="$SRC/edk2:$TOP"
export GCC_AARCH64_PREFIX="${GCC_AARCH64_PREFIX:-aarch64-linux-gnu-}"
mkdir -p "$WORKSPACE"
# edksetup.sh references unset variables, so relax -u while sourcing it
set +u
source "$SRC/edk2/edksetup.sh" BaseTools >/dev/null
set -u
build -a AARCH64 -t GCC -b "$TARGET" -n "$(nproc)" \
      -p Platform/OrangePi/OrangePiZero2W/OrangePiZero2W.dsc
FD="$WORKSPACE/Build/OrangePiZero2W/${TARGET}_GCC/FV/OPIZERO2W_EFI.fd"
ls -la "$FD"
