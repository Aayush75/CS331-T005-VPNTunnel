#!/usr/bin/env bash
# Two-Pi DIRECT baseline: raw Pi A -> Pi B throughput and latency over the Ethernet cable,
# with no tunnel involved. This is the reference to compare bench-pi.sh (laptop-to-laptop
# through the VPN) against: it shows what the cable itself can carry.
#
# Run it ON Pi A. Pi B runs a persistent iperf3 server (no restarts needed between runs):
#
#   # on Pi B
#   iperf3 -s -B 172.31.250.12
#   # on Pi A
#   ./scripts/bench-pi-direct.sh [PI_B_CABLE_IP] [PI_A_CABLE_IP] [--runs N] [--seconds S]
#
# Defaults: Pi B 172.31.250.12, Pi A 172.31.250.11 (the cable addresses in docs/pi-setup.md).
# The cable addresses are the underlay, so this traffic never enters the tunnel whether the
# VPN is up or down.
set -uo pipefail
. "$(dirname "$0")/common.sh"

B_IP="172.31.250.12"
A_IP="172.31.250.11"
case "${1:-}" in --*|"") ;; *) B_IP="$1"; shift ;; esac
case "${1:-}" in --*|"") ;; *) A_IP="$1"; shift ;; esac
RUNS=5; SECONDS_EACH=20
while [ $# -gt 0 ]; do
    case "$1" in
        --runs) RUNS="$2"; shift 2 ;;
        --seconds) SECONDS_EACH="$2"; shift 2 ;;
        *) die "unknown argument: $1" ;;
    esac
done
command -v iperf3 >/dev/null || die "iperf3 not installed on this Pi"
command -v python3 >/dev/null || die "python3 not installed"

ts="$(date -u +%Y%m%d-%H%M%S)"
out="${CODE_DIR}/results/${ts}-pi-direct"
mkdir -p "${out}"
summary="${out}/summary.md"

mbps() {
    python3 -c 'import sys,json;d=json.load(sys.stdin);print(round(d["end"]["sum_received"]["bits_per_second"]/1e6,1))' 2>/dev/null || echo "-"
}

say "direct baseline: ${A_IP} -> ${B_IP} over the cable, ${RUNS} runs x ${SECONDS_EACH}s"
ping -c 2 -W 2 "${B_IP}" >/dev/null 2>&1 || die "cannot reach ${B_IP}; check the cable addresses (docs/pi-setup.md)"

{
    echo "# VPTunnel two-Pi direct baseline (no tunnel)"
    echo
    echo "- Pi A (${A_IP}) -> Pi B (${B_IP}) over the Ethernet cable, ${RUNS} runs x ${SECONDS_EACH} s"
    echo "- Underlay path only: no tunnel, no Wi-Fi, no laptops"
    echo "- Kernel: \`$(uname -r)\` ($(uname -m))"
    echo
    echo "## ping RTT"
    echo '```'
} > "${summary}"
ping -c 50 -i 0.05 "${B_IP}" 2>&1 | tail -3 | tee -a "${summary}"
echo '```' >> "${summary}"

{
    echo
    echo "## TCP (iperf3)"
    echo '| run | forward Mbit/s | reverse Mbit/s |'
    echo '|---|---|---|'
} >> "${summary}"
for i in $(seq 1 "${RUNS}"); do
    say "  TCP run ${i}"
    fwd=$(iperf3 -c "${B_IP}" -B "${A_IP}" -t "${SECONDS_EACH}" -O 2 -J 2>/dev/null | mbps)
    rev=$(iperf3 -c "${B_IP}" -B "${A_IP}" -t "${SECONDS_EACH}" -O 2 -R -J 2>/dev/null | mbps)
    echo "| ${i} | ${fwd} | ${rev} |" >> "${summary}"
done

{
    echo
    echo "Record the cable link speed (\`ethtool eth0\`) alongside these numbers. Compare with"
    echo "\`bench-pi.sh\`: the gap is the tunnel plus the Wi-Fi hops, so read the Wi-Fi rate too."
} >> "${summary}"

cat "${summary}"
say "results in ${out}"
