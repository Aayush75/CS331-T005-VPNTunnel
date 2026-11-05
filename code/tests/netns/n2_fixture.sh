#!/usr/bin/env bash
# AN2: N2 independent multi-peer fixture (11 namespaces), TEST_PLAN.md section 3.3.
# It is the N1 fixture plus gw-c and host-c on the same underlay.
#
#   n2_fixture.sh setup    [RUN_ID]
#   n2_fixture.sh check    [RUN_ID]
#   n2_fixture.sh teardown [RUN_ID]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/netns/fixture_lib.sh
. "${SCRIPT_DIR}/fixture_lib.sh"
N1_SCRIPT="${SCRIPT_DIR}/n1_fixture.sh"

ACTION="${1:-setup}"
RUN_ID="${2:-n2_$$}"

NS_GWC="vpt_${RUN_ID}_gwc"
NS_HC="vpt_${RUN_ID}_hc"
NS_GWA="vpt_${RUN_ID}_gwa"
NS_GWB="vpt_${RUN_ID}_gwb"
NS_UND="vpt_${RUN_ID}_und"
NS_HA="vpt_${RUN_ID}_ha"

fx_require_root

case "${ACTION}" in
teardown | clean)
    echo "==> [N2 Teardown] Removing N2 namespaces for ${RUN_ID}..."
    rc=0
    fx_delete_ns "${NS_GWC}" "${NS_HC}" || rc=1
    bash "${N1_SCRIPT}" teardown "${RUN_ID}" || rc=1
    [ "${rc}" -eq 0 ] && echo "==> [N2 Teardown] Complete."
    exit "${rc}"
    ;;
check)
    bash "${N1_SCRIPT}" check "${RUN_ID}"
    fail=0
    expect() {
        local what=$1 want=$2 rc=0
        shift 2
        "$@" >/dev/null 2>&1 || rc=$?
        if { [ "$want" = 0 ] && [ "$rc" -eq 0 ]; } || { [ "$want" = 1 ] && [ "$rc" -ne 0 ]; }; then
            echo "    [PASS] ${what}"
        else
            echo "    [FAIL] ${what} (rc=${rc})"
            fail=1
        fi
    }
    echo "==> [N2 Check] ${RUN_ID}"
    expect "gw-c reaches gw-a on the underlay" 0 ip netns exec "${NS_GWC}" ping -c1 -W1 172.31.250.11
    expect "gw-c reaches gw-b on the underlay" 0 ip netns exec "${NS_GWC}" ping -c1 -W1 172.31.250.12
    expect "host-c reaches its gateway" 0 ip netns exec "${NS_HC}" ping -c1 -W1 10.77.3.1
    expect "host-c does NOT reach host-a without a tunnel" 1 ip netns exec "${NS_HC}" ping -c1 -W1 10.77.1.10
    expect "host-a does NOT reach host-c without a tunnel" 1 ip netns exec "${NS_HA}" ping -c1 -W1 10.77.3.10
    expect "site C owns 10.77.3.0/24 on gw-c only" 0 ip -n "${NS_GWC}" -4 addr show to 10.77.3.1
    [ "${fail}" -eq 0 ] || exit 1
    echo "==> [N2 Check] PASS"
    exit 0
    ;;
setup) ;;
*)
    echo "usage: $0 setup|check|teardown [RUN_ID]" >&2
    exit 2
    ;;
esac

fx_require_free "${NS_GWC}" "${NS_HC}"

N1_DONE=0
SETUP_OK=0
on_exit() {
    if [ "${SETUP_OK}" -ne 1 ]; then
        echo "==> [N2] setup failed; removing what this run created..." >&2
        fx_teardown_recorded || true
        if [ "${N1_DONE}" -eq 1 ]; then
            bash "${N1_SCRIPT}" teardown "${RUN_ID}" || true
        fi
    fi
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

echo "=================================================================="
echo "==> Provisioning AN2 N2 Multi-Peer Fixture (11 Namespaces), run ${RUN_ID}"
echo "=================================================================="

# The base 9 namespaces; n1_fixture.sh refuses an existing run and cleans up after itself.
bash "${N1_SCRIPT}" setup "${RUN_ID}"
N1_DONE=1

fx_ns_add "${NS_GWC}" "${NS_HC}"
fx_no_forwarding "${NS_HC}"

# gw-c joins the underlay bridge (outer address 172.31.250.13/24).
fx_veth "${NS_GWC}" veth_gwc_out "${NS_UND}" veth_und_c
ip -n "${NS_UND}" link set veth_und_c master br_und
ip -n "${NS_UND}" link set veth_und_c up
ip -n "${NS_GWC}" addr add 172.31.250.13/24 dev veth_gwc_out
ip -n "${NS_GWC}" link set veth_gwc_out up
ip netns exec "${NS_GWC}" sysctl -q -w net.ipv4.ip_forward=1

# host-c (10.77.3.10) behind gw-c (10.77.3.1).
fx_veth "${NS_HC}" veth_hc "${NS_GWC}" veth_gwc_lan
ip -n "${NS_GWC}" addr add 10.77.3.1/24 dev veth_gwc_lan
ip -n "${NS_GWC}" link set veth_gwc_lan up
ip -n "${NS_HC}" addr add 10.77.3.10/24 dev veth_hc
ip -n "${NS_HC}" link set veth_hc up
ip -n "${NS_HC}" route add 10.77.2.0/24 via 10.77.3.1

# Return route for the site C prefix on gw-b. A failure here is a fixture error.
ip -n "${NS_GWB}" route add 10.77.3.0/24 via 172.31.250.13

SETUP_OK=1
echo "==> [PASS] N2 Multi-Peer Fixture ready."
echo "    - Site A: 10.77.1.0/24 (gw-a: 172.31.250.11)"
echo "    - Site B: 10.77.2.0/24 (gw-b: 172.31.250.12)"
echo "    - Site C: 10.77.3.0/24 (gw-c: 172.31.250.13)"
echo "    - Underlay: ${NS_UND} bridging 3 outer gateway endpoints"
