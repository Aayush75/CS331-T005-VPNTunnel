#!/usr/bin/env bash
# Namespace throughput / latency benchmark: VPN vs a direct no-tunnel baseline, in network
# namespaces on this machine. Reproducible by a TA with no hardware, on the reference VM.
#
#   sudo ./scripts/bench-netns.sh [--runs N] [--seconds S] [--udp-rate MBIT]
#
# Results land in code/results/<timestamp>-netns/summary.md.
set -uo pipefail
. "$(dirname "$0")/common.sh"
require_root
require_linux_618
VPTCTL="$(build_vptctl)" || exit 1
export VPTCTL
exec env VPT1_DISPOSABLE_VM=yes VPTCTL="${VPTCTL}" \
     python3 "${CODE_DIR}/tools/bench_netns.py" "$@"
