#!/usr/bin/env bash
# Packet-size sweep over the namespace tunnel: delivered goodput and loss versus UDP
# datagram size, showing the 72-byte per-packet overhead and the 1428-byte inner MTU.
#
#   sudo ./scripts/bench-packetsize.sh [--seconds S] [--rate MBIT]
#
# Results land in code/results/<timestamp>-packet/summary.md.
set -uo pipefail
. "$(dirname "$0")/common.sh"
require_root
require_linux_618
VPTCTL="$(build_vptctl)" || exit 1
export VPTCTL
exec env VPT1_DISPOSABLE_VM=yes VPTCTL="${VPTCTL}" \
     python3 "${CODE_DIR}/tools/bench_packetsize.py" "$@"
