#!/usr/bin/env bash
# AN2: N1 multi-host two-site fixture (9 namespaces), TEST_PLAN.md section 3.2.
#
#   n1_fixture.sh setup    [RUN_ID]   create the fixture (refuses an existing RUN_ID)
#   n1_fixture.sh check    [RUN_ID]   assert its topology and isolation properties
#   n1_fixture.sh teardown [RUN_ID]   remove it and verify the removal
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/netns/fixture_lib.sh
. "${SCRIPT_DIR}/fixture_lib.sh"

ACTION="${1:-setup}"
RUN_ID="${2:-n1_$$}"

NS_HA="vpt_${RUN_ID}_ha"
NS_HA2="vpt_${RUN_ID}_ha2"
NS_LANA="vpt_${RUN_ID}_lana"
NS_GWA="vpt_${RUN_ID}_gwa"
NS_UND="vpt_${RUN_ID}_und"
NS_GWB="vpt_${RUN_ID}_gwb"
NS_LANB="vpt_${RUN_ID}_lanb"
NS_HB="vpt_${RUN_ID}_hb"
NS_HB2="vpt_${RUN_ID}_hb2"
ALL_NS=("${NS_HA}" "${NS_HA2}" "${NS_LANA}" "${NS_GWA}" "${NS_UND}" "${NS_GWB}" "${NS_LANB}" "${NS_HB}" "${NS_HB2}")

fx_require_root

case "${ACTION}" in
teardown | clean)
    echo "==> [N1 Teardown] Removing ${#ALL_NS[@]} namespaces for ${RUN_ID}..."
    fx_delete_ns "${ALL_NS[@]}"
    echo "==> [N1 Teardown] Complete."
    exit 0
    ;;
check)
    fail=0
    expect() { # description, expected rc (0 or 1), command...
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
    echo "==> [N1 Check] ${RUN_ID}"
    expect "host-a reaches host-a2 across the site-A bridge" 0 ip netns exec "${NS_HA}" ping -c1 -W1 10.77.1.11
    expect "host-a reaches its gateway" 0 ip netns exec "${NS_HA}" ping -c1 -W1 10.77.1.1
    expect "host-b reaches host-b2 across the site-B bridge" 0 ip netns exec "${NS_HB}" ping -c1 -W1 10.77.2.11
    expect "gateways reach each other on the underlay" 0 ip netns exec "${NS_GWA}" ping -c1 -W1 172.31.250.12
    expect "host-a does NOT reach host-b without a tunnel" 1 ip netns exec "${NS_HA}" ping -c1 -W1 10.77.2.10
    expect "forwarding is on in the gateways only (gw-a)" 0 test "$(ip netns exec "${NS_GWA}" sysctl -n net.ipv4.ip_forward)" = 1
    expect "forwarding is off in host-a" 0 test "$(ip netns exec "${NS_HA}" sysctl -n net.ipv4.ip_forward)" = 0
    expect "the underlay namespace has no IPv4 address" 0 test -z "$(ip -n "${NS_UND}" -4 -o addr show scope global)"
    [ "${fail}" -eq 0 ] || exit 1
    echo "==> [N1 Check] PASS"
    exit 0
    ;;
setup) ;;
*)
    echo "usage: $0 setup|check|teardown [RUN_ID]" >&2
    exit 2
    ;;
esac

fx_require_free "${ALL_NS[@]}"

SETUP_OK=0
on_exit() {
    if [ "${SETUP_OK}" -ne 1 ]; then
        echo "==> [N1] setup failed; removing what this run created..." >&2
        fx_teardown_recorded || true
    fi
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

echo "=================================================================="
echo "==> Provisioning AN2 N1 Multi-Host Fixture (9 Namespaces), run ${RUN_ID}"
echo "=================================================================="

fx_ns_add "${ALL_NS[@]}"
fx_no_forwarding "${NS_HA}" "${NS_HA2}" "${NS_LANA}" "${NS_UND}" "${NS_LANB}" "${NS_HB}" "${NS_HB2}"

# Switch-only bridges (no addresses) in lan-a, underlay and lan-b.
ip -n "${NS_LANA}" link add br_lana type bridge
ip -n "${NS_LANA}" link set br_lana up
ip -n "${NS_UND}" link add br_und type bridge
ip -n "${NS_UND}" link set br_und up
ip -n "${NS_LANB}" link add br_lanb type bridge
ip -n "${NS_LANB}" link set br_lanb up

# attach NS_HOST IF_HOST NS_BR IF_BR BRIDGE [ADDR/LEN] : a host-side veth into a bridge.
attach() {
    fx_veth "$1" "$2" "$3" "$4"
    ip -n "$3" link set "$4" master "$5"
    ip -n "$3" link set "$4" up
    ip -n "$1" link set "$2" up
    if [ -n "${6:-}" ]; then
        ip -n "$1" addr add "$6" dev "$2"
    fi
}

# Site A: host-a, host-a2 and gw-a (10.77.1.1) on br_lana.
attach "${NS_HA}" veth_ha "${NS_LANA}" veth_lana_ha br_lana 10.77.1.10/24
attach "${NS_HA2}" veth_ha2 "${NS_LANA}" veth_lana_ha2 br_lana 10.77.1.11/24
attach "${NS_GWA}" veth_gwa_lan "${NS_LANA}" veth_lana_gwa br_lana 10.77.1.1/24
ip -n "${NS_HA}" route add 10.77.2.0/24 via 10.77.1.1
ip -n "${NS_HA2}" route add 10.77.2.0/24 via 10.77.1.1
ip netns exec "${NS_GWA}" sysctl -q -w net.ipv4.ip_forward=1

# Underlay: gw-a and gw-b outer addresses on br_und.
attach "${NS_GWA}" veth_gwa_out "${NS_UND}" veth_und_a br_und 172.31.250.11/24
attach "${NS_GWB}" veth_gwb_out "${NS_UND}" veth_und_b br_und 172.31.250.12/24

# Site B: gw-b (10.77.2.1), host-b and host-b2 on br_lanb.
attach "${NS_GWB}" veth_gwb_lan "${NS_LANB}" veth_lanb_gwb br_lanb 10.77.2.1/24
attach "${NS_HB}" veth_hb "${NS_LANB}" veth_lanb_hb br_lanb 10.77.2.10/24
attach "${NS_HB2}" veth_hb2 "${NS_LANB}" veth_lanb_hb2 br_lanb 10.77.2.11/24
ip -n "${NS_HB}" route add 10.77.1.0/24 via 10.77.2.1
ip -n "${NS_HB2}" route add 10.77.1.0/24 via 10.77.2.1
ip netns exec "${NS_GWB}" sysctl -q -w net.ipv4.ip_forward=1

SETUP_OK=1
echo "==> [PASS] N1 Multi-Host Fixture ready."
echo "    - Site A: ${NS_HA} (10.77.1.10), ${NS_HA2} (10.77.1.11) via ${NS_GWA} (10.77.1.1)"
echo "    - Underlay: ${NS_UND} bridging ${NS_GWA} (172.31.250.11) and ${NS_GWB} (172.31.250.12)"
echo "    - Site B: ${NS_HB} (10.77.2.10), ${NS_HB2} (10.77.2.11) via ${NS_GWB} (10.77.2.1)"
