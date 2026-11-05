#!/usr/bin/env bash
# Build everything: the kernel module (release + debug) and the vptctl controller.
# Run on a Linux 6.18.y host (the reference VM) with matching kernel headers and a Rust toolchain.
#
#   sudo -v            # not needed to build; building is unprivileged
#   ./scripts/build.sh
set -uo pipefail
. "$(dirname "$0")/common.sh"

require_linux_618

say "kernel module (release)"
make -C "${CODE_DIR}/kernel" clean >/dev/null 2>&1 || true
make -C "${CODE_DIR}/kernel" W=1 || die "release module build failed"

say "kernel module (debug, VPT1_DEBUG=1) — adds the in-kernel unit-test modules"
make -C "${CODE_DIR}/kernel" clean >/dev/null 2>&1 || true
make -C "${CODE_DIR}/kernel" W=1 VPT1_DEBUG=1 || die "debug module build failed"

say "controller (vptctl, release)"
( cd "${CODE_DIR}/control" && cargo build --release --locked ) || die "vptctl build failed"

say "done"
ls -1 "${CODE_DIR}"/kernel/vpt1_*.ko 2>/dev/null | sed 's/^/  module: /'
echo "  vptctl:  ${CODE_DIR}/control/target/release/vptctl"
