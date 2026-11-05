#!/usr/bin/env bash
# Build the module and controller natively ON a Raspberry Pi 5 (aarch64, 16 KiB pages,
# Linux 6.18.y vendor kernel). Run this on each Pi after cloning the repo there.
#
# The `ring` crate (a vptctl dependency) needs a C compiler, so vptctl is built natively
# on the Pi, not cross-compiled. Takes about a minute on a Pi 5.
#
#   ./scripts/pi-build.sh
set -uo pipefail
. "$(dirname "$0")/common.sh"

case "$(uname -m)" in
    aarch64|arm64) ;;
    *) warn "this is meant to run on the Pi (aarch64); found $(uname -m)" ;;
esac
require_linux_618

say "kernel module"
make -C "${CODE_DIR}/kernel" || die "module build failed (need 6.18 headers under /lib/modules/$(uname -r)/build)"

say "controller"
command -v cargo >/dev/null || die "cargo not found; install Rust (rustup) first"
( cd "${CODE_DIR}/control" && cargo build --release --locked ) || die "vptctl build failed"

say "built:"
echo "  module: ${CODE_DIR}/kernel/vpt1_foundation.ko"
echo "  vptctl: ${CODE_DIR}/control/target/release/vptctl"
echo
echo "Next: scripts/pi-demo.sh on each Pi (see docs/pi-setup.md)."
