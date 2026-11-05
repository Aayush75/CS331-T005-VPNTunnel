#!/usr/bin/env bash
# Bring the VPN up or down on one Raspberry Pi for the two-Pi demonstration.
# Thin wrapper over infra/gateway/vpt-gateway.sh so the demo is one command per Pi.
#
# Full hardware setup (packages, keys, cable address, access point) is in docs/pi-setup.md.
# Start Pi B (the responder) before Pi A (the initiator).
#
#   sudo ./scripts/pi-demo.sh up   a      # on Pi A, using infra/gateway/demo-pi-a.toml
#   sudo ./scripts/pi-demo.sh up   b      # on Pi B, using demo-pi-b.toml
#   sudo ./scripts/pi-demo.sh status a
#   sudo ./scripts/pi-demo.sh down a
set -uo pipefail
. "$(dirname "$0")/common.sh"

require_root
action="${1:-}"
side="${2:-}"
case "${action}" in up|down|status) ;; *) die "usage: pi-demo.sh {up|down|status} {a|b}" ;; esac
case "${side}"   in a|b) ;; *) die "usage: pi-demo.sh {up|down|status} {a|b}" ;; esac

cfg="${CODE_DIR}/infra/gateway/demo-pi-${side}.toml"
[ -f "${cfg}" ] || die "missing ${cfg}"

VPTCTL="$(build_vptctl)" || exit 1
MODULE="$(build_module)" || exit 1
export VPTCTL VPT1_MODULE="${MODULE}"

say "${action} VPN on Pi ${side} (config: ${cfg})"
case "${action}" in
    up)     exec "${CODE_DIR}/infra/gateway/vpt-gateway.sh" up     "${cfg}" --wait 20 ;;
    down)   exec "${CODE_DIR}/infra/gateway/vpt-gateway.sh" down   "${cfg}" ;;
    status) exec "${CODE_DIR}/infra/gateway/vpt-gateway.sh" status "${cfg}" ;;
esac
