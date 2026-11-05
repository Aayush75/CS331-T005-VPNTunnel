#!/usr/bin/env bash
# Reproduce the core demonstration in Linux network namespaces, with no hardware:
# an ordinary client on one virtual LAN reaches an HTTP/ping/UDP service on another
# virtual LAN ONLY when the VPN is running, and cannot when it is off.
#
# This drives the real gateway control path (vptctl + the kernel module) across five
# namespaces (client, gateway A, underlay, gateway B, service) and checks reachability
# with the VPN down, up, and down again — the before/after the project is about.
#
# Must run as root on a DISPOSABLE Linux 6.18.y machine (the reference VM).
#
#   sudo ./scripts/demo-netns.sh
set -uo pipefail
. "$(dirname "$0")/common.sh"

require_root
require_linux_618

VPTCTL="$(build_vptctl)" || exit 1
MODULE="$(build_module)" || exit 1
export VPTCTL

say "loading the kernel module"
modprobe chacha20poly1305 2>/dev/null || true
modprobe udp_tunnel 2>/dev/null || true
rmmod vpt1_foundation 2>/dev/null || true
insmod "${MODULE}" || die "insmod failed"

say "running the namespace demonstration (VPN off -> on -> off, both directions)"
python3 "${CODE_DIR}/tests/netns/demo_reachability.py" "${VPTCTL}"
rc=$?

rmmod vpt1_foundation 2>/dev/null || true

echo
if [ "${rc}" -eq 0 ]; then
    say "DEMO PASSED: the service is unreachable without the VPN and reachable through it"
else
    say "DEMO FAILED (exit ${rc})"
fi
exit "${rc}"
