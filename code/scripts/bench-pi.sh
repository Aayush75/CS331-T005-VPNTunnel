#!/usr/bin/env bash
# Two-Pi (physical) throughput / latency benchmark, run from laptop A against laptop B
# while the VPN is up on both Pis. Measures the real path over the Pi-to-Pi cable and the
# laptops' Wi-Fi, which the namespace benchmark cannot.
#
# Prerequisites: the two-Pi demo is up (scripts/pi-demo.sh up on each Pi), both laptops are
# on their Pi's Wi-Fi, and laptop B runs an iperf3 server:
#
#   # on laptop B
#   iperf3 -s -1 -B <LAPTOP_B_IP>
#   # on laptop A
#   ./scripts/bench-pi.sh <LAPTOP_B_IP> [LAPTOP_A_IP] [--runs N] [--seconds S]
#
# Run it once with the VPN UP and once with it DOWN-at-the-Pis to contrast reachability
# (with the VPN down the runs fail, which is the point of the demo, not a benchmark number).
set -uo pipefail
. "$(dirname "$0")/common.sh"

B_IP="${1:-}"
[ -n "${B_IP}" ] || die "usage: bench-pi.sh LAPTOP_B_IP [LAPTOP_A_IP] [--runs N] [--seconds S]"
shift
A_BIND=""
case "${1:-}" in
    --*|"") ;;            # next is a flag or nothing
    *) A_BIND="$1"; shift ;;
esac
RUNS=5; SECONDS_EACH=20
while [ $# -gt 0 ]; do
    case "$1" in
        --runs) RUNS="$2"; shift 2 ;;
        --seconds) SECONDS_EACH="$2"; shift 2 ;;
        *) die "unknown argument: $1" ;;
    esac
done
command -v iperf3 >/dev/null || die "iperf3 not installed on this laptop"

ts="$(date -u +%Y%m%d-%H%M%S)"
out="${CODE_DIR}/results/${ts}-pi"
mkdir -p "${out}"
bind_opt=(); [ -n "${A_BIND}" ] && bind_opt=(-B "${A_BIND}")
summary="${out}/summary.md"

say "two-Pi benchmark: A -> ${B_IP}, ${RUNS} runs x ${SECONDS_EACH}s"
{
    echo "# VPTunnel two-Pi benchmark"
    echo
    echo "- laptop A -> laptop B (${B_IP}) over the VPN, ${RUNS} runs x ${SECONDS_EACH} s"
    echo "- physical path: Wi-Fi (laptop<->Pi) + Ethernet cable (Pi<->Pi)"
    echo
    echo "## ping RTT"
    echo '```'
} > "${summary}"

ping -c 50 -i 0.05 "${B_IP}" 2>&1 | tail -3 | tee -a "${summary}"
echo '```' >> "${summary}"

echo "## TCP (iperf3)" >> "${summary}"
echo '| run | forward Mbit/s | reverse Mbit/s |' >> "${summary}"
echo '|---|---|---|' >> "${summary}"
for i in $(seq 1 "${RUNS}"); do
    say "  TCP run ${i} (B must have 'iperf3 -s -1' running each time)"
    fwd=$(iperf3 -c "${B_IP}" "${bind_opt[@]}" -t "${SECONDS_EACH}" -O 2 -J 2>/dev/null \
          | python3 -c 'import sys,json;d=json.load(sys.stdin);print(round(d["end"]["sum_received"]["bits_per_second"]/1e6,1))' 2>/dev/null || echo "-")
    say "    restart the iperf3 server on B, then press Enter for the reverse run"; read -r _
    rev=$(iperf3 -c "${B_IP}" "${bind_opt[@]}" -t "${SECONDS_EACH}" -O 2 -R -J 2>/dev/null \
          | python3 -c 'import sys,json;d=json.load(sys.stdin);print(round(d["end"]["sum_received"]["bits_per_second"]/1e6,1))' 2>/dev/null || echo "-")
    echo "| ${i} | ${fwd} | ${rev} |" >> "${summary}"
    [ "${i}" -lt "${RUNS}" ] && { say "    restart the iperf3 server on B, then press Enter for the next run"; read -r _; }
done

{
    echo
    echo "Record the negotiated Wi-Fi rate and the cable link speed alongside these numbers"
    echo "(\`iw dev wlan0 link\` on each Pi, \`ethtool eth0\`); throughput on Wi-Fi varies with"
    echo "signal and contention, so a single run is not a stable figure."
} >> "${summary}"

say "results in ${out}"
