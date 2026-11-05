#!/usr/bin/env bash
# Cross-build the kernel module for ARM64 (Raspberry Pi 5 / 16 KiB pages) from an x86-64
# host, and run the build-prerequisite inventory and the page-size assumption audit.
# BUILD + inventory only: a cross-build host has no ARM64 runtime, so it never claims a
# runtime pass. The runtime lane is the Pi itself (scripts/pi-build.sh + pi-demo.sh).
#
# Needs an aarch64 cross toolchain and a prepared ARM64 6.18 kernel build tree:
#
#   KDIR=/path/to/arm64-6.18-build \
#   CROSS_COMPILE=aarch64-linux-gnu- \
#   [CONFIG=/path/to/arm64/.config] [PAGE_SHIFT=14] \
#   ./scripts/arm64-build.sh
set -uo pipefail
. "$(dirname "$0")/common.sh"

exec bash "${CODE_DIR}/tools/arm64_build.sh"
