#!/usr/bin/env bash
# AN2: N0 minimal two-site fixture, independent HTTP/packet oracles and the Gate G0
# baseline. TEST_PLAN.md sections 3.1, 4.1 and 4.2; TEAM_WORK_PLAN.md sections 3.4, 7.
#
# Phase 1 (baseline): with no tunnel, the remote private service is unreachable, the
#   underlay carries no private packets, and a local positive control is healthy.
# Phase 2 (oracle self-check): static routes are added across the underlay so private
#   traffic crosses it in the clear. The request must then SUCCEED and the leak
#   oracle must REPORT the leaked packets. This proves both oracles can fail, so a
#   phase-1 PASS means something.
# The result ledger is written only after teardown has been verified.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# shellcheck source=tests/netns/fixture_lib.sh
. "${SCRIPT_DIR}/fixture_lib.sh"
fx_require_root

ORACLE_SERVER="${REPO_ROOT}/tools/oracles/http_server.py"
ORACLE_CLIENT="${REPO_ROOT}/tools/oracles/http_client.py"

for cmd in ip tcpdump python3 sha256sum; do
    command -v "${cmd}" >/dev/null 2>&1 || { echo "ERROR: required command '${cmd}' is missing." >&2; exit 2; }
done

RUN_ID="n0_${$}_$(date +%s)"
ARTIFACT_DIR="${AN2_ARTIFACT_DIR:-/tmp/an2_artifacts_${RUN_ID}}"
mkdir -p "${ARTIFACT_DIR}"

NS_HOST_A="vpt_${RUN_ID}_ha"
NS_GW_A="vpt_${RUN_ID}_gwa"
NS_UNDERLAY="vpt_${RUN_ID}_und"
NS_GW_B="vpt_${RUN_ID}_gwb"
NS_HOST_B="vpt_${RUN_ID}_hb"

SERVER_PID=""
CAPTURE_PIDS=()
START_TIME=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
declare -A RESULT=()
OVERALL=FAIL
TEARDOWN=NOT_RUN

fx_require_free "${NS_HOST_A}" "${NS_GW_A}" "${NS_UNDERLAY}" "${NS_GW_B}" "${NS_HOST_B}"

stop_background() {
    local pid
    for pid in "${CAPTURE_PIDS[@]}" ${SERVER_PID:+"${SERVER_PID}"}; do
        if kill -0 "${pid}" 2>/dev/null; then
            kill -TERM "${pid}" 2>/dev/null || true
            wait "${pid}" 2>/dev/null || true
        fi
    done
    CAPTURE_PIDS=()
    SERVER_PID=""
}

write_results() {
    local und_sha=""
    [ -s "${ARTIFACT_DIR}/underlay_baseline_a.pcap" ] && und_sha=$(sha256sum "${ARTIFACT_DIR}/underlay_baseline_a.pcap" | awk '{print $1}')
    {
        echo "{"
        echo "  \"run_id\": \"${RUN_ID}\","
        echo "  \"fixture\": \"N0\","
        echo "  \"start_time\": \"${START_TIME}\","
        echo "  \"end_time\": \"$(date -u +"%Y-%m-%dT%H:%M:%SZ")\","
        echo "  \"kernel_release\": \"$(uname -r)\","
        echo "  \"assertions\": {"
        local keys=(root_isolation underlay_carrier_connectivity local_positive_control
            disabled_tunnel_negative_control underlay_capture_not_empty
            underlay_zero_leakage oracle_detects_cleartext_success
            oracle_detects_underlay_leak baseline_restored teardown_clean)
        local i k
        for i in "${!keys[@]}"; do
            k=${keys[$i]}
            printf '    "%s": "%s"%s\n' "${k}" "${RESULT[${k}]:-NOT_RUN}" "$([ "${i}" -lt $((${#keys[@]} - 1)) ] && echo ,)"
        done
        echo "  },"
        echo "  \"overall_result\": \"${OVERALL}\","
        echo "  \"artifacts\": {"
        echo "    \"directory\": \"${ARTIFACT_DIR}\","
        echo "    \"underlay_baseline_port_a_pcap_sha256\": \"${und_sha}\""
        echo "  }"
        echo "}"
    } >"${ARTIFACT_DIR}/results.json"
}

# Runs on every exit. Verifies teardown, then writes the ledger with the real outcome.
finish() {
    local rc=$?
    trap - EXIT
    stop_background
    if fx_teardown_recorded; then
        RESULT[teardown_clean]=PASS
        TEARDOWN=PASS
    else
        RESULT[teardown_clean]=FAIL
        TEARDOWN=FAIL
        rc=1
    fi
    [ "${rc}" -eq 0 ] && OVERALL=PASS || OVERALL=FAIL
    write_results
    echo "==> [Teardown] ${TEARDOWN}. Results ledger: ${ARTIFACT_DIR}/results.json (overall ${OVERALL})"
    exit "${rc}"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

fail() {
    echo "    [ASSERTION FAILED] $*" >&2
    exit 1
}

echo "=================================================================="
echo "==> Starting AN2 N0 Baseline, Captures & Negative Isolation Test"
echo "==> Run ID: ${RUN_ID}"
echo "=================================================================="

# 1. Root-namespace isolation: the fixture must never leave links or routes behind in root.
echo "--> [Check 1] Root namespace isolation (TEST_PLAN.md section 2)..."
if ip route show | grep -E -q '^10\.77\.[12]\.0/24'; then
    fail "root namespace already routes into the fixture subnets"
fi
ROOT_LINKS_BEFORE=$(ip -o link show | awk -F': ' '{print $2}' | sort)
RESULT[root_isolation]=PASS

# 2. Namespaces and links. Links are created inside their namespaces, never in root.
echo "--> [Check 2] Creating 5 namespaces and the unnumbered underlay bridge..."
fx_ns_add "${NS_HOST_A}" "${NS_GW_A}" "${NS_UNDERLAY}" "${NS_GW_B}" "${NS_HOST_B}"
fx_no_forwarding "${NS_HOST_A}" "${NS_UNDERLAY}" "${NS_HOST_B}"
ip -n "${NS_UNDERLAY}" link add name br_und type bridge
ip -n "${NS_UNDERLAY}" link set br_und up

fx_veth "${NS_HOST_A}" veth_ha "${NS_GW_A}" veth_gwa_lan
fx_veth "${NS_GW_A}" veth_gwa_out "${NS_UNDERLAY}" veth_und_a
fx_veth "${NS_GW_B}" veth_gwb_out "${NS_UNDERLAY}" veth_und_b
fx_veth "${NS_GW_B}" veth_gwb_lan "${NS_HOST_B}" veth_hb
for port in veth_und_a veth_und_b; do
    ip -n "${NS_UNDERLAY}" link set "${port}" master br_und
    ip -n "${NS_UNDERLAY}" link set "${port}" up
done

cfg() { # NS IF ADDR
    ip -n "$1" addr add "$3" dev "$2"
    ip -n "$1" link set "$2" mtu 1500 up
}
cfg "${NS_HOST_A}" veth_ha 10.77.1.10/24
cfg "${NS_GW_A}" veth_gwa_lan 10.77.1.1/24
cfg "${NS_GW_A}" veth_gwa_out 172.31.250.11/24
cfg "${NS_GW_B}" veth_gwb_out 172.31.250.12/24
cfg "${NS_GW_B}" veth_gwb_lan 10.77.2.1/24
cfg "${NS_HOST_B}" veth_hb 10.77.2.10/24
ip -n "${NS_HOST_A}" route add 10.77.2.0/24 via 10.77.1.1
ip -n "${NS_HOST_B}" route add 10.77.1.0/24 via 10.77.2.1
ip netns exec "${NS_GW_A}" sysctl -q -w net.ipv4.ip_forward=1
ip netns exec "${NS_GW_B}" sysctl -q -w net.ipv4.ip_forward=1

# start_capture NS IF FILE : packet-buffered capture; waits until it is really running.
start_capture() {
    # --immediate-mode: without it libpcap delivers packets in timed batches, and a
    # capture stopped inside the batch window silently loses them (observed).
    ip netns exec "$1" tcpdump --immediate-mode -U -i "$2" -n -s 0 -w "$3" >/dev/null 2>&1 &
    CAPTURE_PIDS+=("$!")
    local tries=0
    while [ ! -s "$3" ] && [ "${tries}" -lt 50 ]; do
        sleep 0.1
        tries=$((tries + 1))
    done
    [ -s "$3" ] || fail "capture on $1/$2 did not start"
}

# stop_capture PID : stop one capture and wait for it, so the file is complete.
stop_capture() {
    sleep 0.3
    kill -TERM "$1" 2>/dev/null || true
    wait "$1" 2>/dev/null || true
}

# private_packets PCAP : number of IPv4 packets to or from 10.77.0.0/16. A tcpdump
# failure is an error, never "zero".
private_packets() {
    local out
    out=$(tcpdump -r "$1" -n -q 'ip and net 10.77.0.0/16' 2>/dev/null) || fail "tcpdump could not read $1"
    printf '%s' "${out}" | grep -c . || true
}

total_packets() {
    local out
    out=$(tcpdump -r "$1" -n -q 2>/dev/null) || fail "tcpdump could not read $1"
    printf '%s' "${out}" | grep -c . || true
}

# The underlay is watched on both bridge PORTS. A capture on the bridge device itself
# does not see frames forwarded between ports (verified: it shows only multicast the
# bridge receives), so it could never observe a leak.
start_underlay_captures() { # SUFFIX ; sets UND_PIDS
    start_capture "${NS_UNDERLAY}" veth_und_a "${ARTIFACT_DIR}/underlay_$1_a.pcap"
    UND_PIDS=("${CAPTURE_PIDS[$((${#CAPTURE_PIDS[@]} - 1))]}")
    start_capture "${NS_UNDERLAY}" veth_und_b "${ARTIFACT_DIR}/underlay_$1_b.pcap"
    UND_PIDS+=("${CAPTURE_PIDS[$((${#CAPTURE_PIDS[@]} - 1))]}")
}
stop_underlay_captures() {
    local pid
    for pid in "${UND_PIDS[@]}"; do stop_capture "${pid}"; done
}
sum_packets() { # FUNC SUFFIX : total over both ports
    local n=0 f
    for f in a b; do n=$((n + $("$1" "${ARTIFACT_DIR}/underlay_$2_${f}.pcap"))); done
    echo "${n}"
}

echo "--> [Check 3] Starting packet captures (host-a LAN, both underlay ports, host-b LAN)..."
start_capture "${NS_HOST_A}" veth_ha "${ARTIFACT_DIR}/lana.pcap"
start_underlay_captures baseline
start_capture "${NS_HOST_B}" veth_hb "${ARTIFACT_DIR}/lanb.pcap"

echo "--> [Check 4] Outer underlay carrier connectivity (172.31.250.11 <-> 172.31.250.12)..."
ip netns exec "${NS_GW_A}" ping -c 2 -W 1 172.31.250.12 >/dev/null || fail "underlay carrier is not functional"
RESULT[underlay_carrier_connectivity]=PASS

echo "--> [Check 5] HTTP service oracle on host-b, local positive control from gw-b..."
ip netns exec "${NS_HOST_B}" python3 "${ORACLE_SERVER}" --host 10.77.2.10 --port 8080 --server-id host-b \
    >"${ARTIFACT_DIR}/oracle_server.log" 2>&1 &
SERVER_PID=$!
sleep 0.5
kill -0 "${SERVER_PID}" 2>/dev/null || fail "HTTP oracle server did not start"
ip netns exec "${NS_GW_B}" python3 "${ORACLE_CLIENT}" http://10.77.2.10:8080 \
    --expect-server host-b --expect-client-ip 10.77.2.1 --timeout 2.0 \
    >"${ARTIFACT_DIR}/client_positive_gw_b.json" || fail "local positive control failed"
RESULT[local_positive_control]=PASS

# The expected failure must be a network failure (no route / timeout), not a typo,
# a missing interpreter or a dead server.
expect_network_failure() { # JSON file
    python3 - "$1" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
err = d.get("error") or ""
ok = (not d.get("success")) and any(s in err for s in (
    "Network is unreachable", "No route to host", "timed out", "Connection refused"))
sys.exit(0 if ok else 1)
PY
}

echo "--> [Check 6] Fail-closed negative control (no tunnel, host-a -> host-b)..."
set +e
ip netns exec "${NS_HOST_A}" python3 "${ORACLE_CLIENT}" http://10.77.2.10:8080 --timeout 1.5 \
    >"${ARTIFACT_DIR}/client_negative_host_a.json" 2>&1
set -e
expect_network_failure "${ARTIFACT_DIR}/client_negative_host_a.json" ||
    fail "negative control did not fail with a network error: $(tr -d '\n' <"${ARTIFACT_DIR}/client_negative_host_a.json")"
RESULT[disabled_tunnel_negative_control]=PASS

echo "--> Analyzing the baseline underlay capture..."
stop_underlay_captures
TOTAL=$(sum_packets total_packets baseline)
[ "${TOTAL}" -gt 0 ] || fail "underlay capture is empty; the leak check would prove nothing"
RESULT[underlay_capture_not_empty]=PASS
LEAKED=$(sum_packets private_packets baseline)
[ "${LEAKED}" -eq 0 ] || fail "${LEAKED} private packets leaked onto the underlay in the baseline"
RESULT[underlay_zero_leakage]=PASS
echo "    [PASS] ${TOTAL} underlay packets captured, 0 private."

# Phase 2: deliberately route private traffic across the underlay in the clear.
echo "--> [Phase 2] Oracle self-check: cleartext routes across the underlay..."
start_underlay_captures cleartext
ip -n "${NS_GW_A}" route add 10.77.2.0/24 via 172.31.250.12
ip -n "${NS_GW_B}" route add 10.77.1.0/24 via 172.31.250.11
ip netns exec "${NS_HOST_A}" python3 "${ORACLE_CLIENT}" http://10.77.2.10:8080 \
    --expect-server host-b --expect-client-ip 10.77.1.10 --timeout 2.0 \
    >"${ARTIFACT_DIR}/client_cleartext_host_a.json" ||
    fail "cleartext request did not succeed, so the negative control proved nothing"
RESULT[oracle_detects_cleartext_success]=PASS
stop_underlay_captures
CLEAR_LEAKED=$(sum_packets private_packets cleartext)
[ "${CLEAR_LEAKED}" -gt 0 ] || fail "the leak oracle did not flag cleartext private traffic on the underlay"
RESULT[oracle_detects_underlay_leak]=PASS
echo "    [PASS] oracle succeeded in the clear and the leak check saw ${CLEAR_LEAKED} private packets."

# Remove the mutation and confirm the baseline is restored.
ip -n "${NS_GW_A}" route del 10.77.2.0/24 via 172.31.250.12
ip -n "${NS_GW_B}" route del 10.77.1.0/24 via 172.31.250.11
set +e
ip netns exec "${NS_HOST_A}" python3 "${ORACLE_CLIENT}" http://10.77.2.10:8080 --timeout 1.5 \
    >"${ARTIFACT_DIR}/client_restored_host_a.json" 2>&1
set -e
expect_network_failure "${ARTIFACT_DIR}/client_restored_host_a.json" || fail "baseline was not restored after the mutation"
RESULT[baseline_restored]=PASS

# The fixture must not have left anything in the root namespace.
# Every fixture link is created inside a namespace (veth_*, br_*), so none may exist in the root
# namespace. Other links can come and go on a shared runner (containers, VPN clients) while the
# test runs; those are reported but are not the fixture's doing.
ROOT_LINKS_AFTER=$(ip -o link show | awk -F': ' '{print $2}' | sort)
if [ "${ROOT_LINKS_BEFORE}" != "${ROOT_LINKS_AFTER}" ]; then
    CHANGED=$(diff <(echo "${ROOT_LINKS_BEFORE}") <(echo "${ROOT_LINKS_AFTER}") | grep '^[<>]' || true)
    echo "    root namespace links changed during the test:"
    echo "${CHANGED}" | sed 's/^/      /'
    if echo "${CHANGED}" | grep -Eq '^[<>] (veth_|br_)'; then
        fail "a fixture link leaked into the root namespace"
    fi
    echo "    (none is a fixture link; ignoring)"
fi

echo "=================================================================="
echo "==> [PASS] AN2 N0 baseline and oracle self-check passed; teardown follows."
echo "=================================================================="
