#!/usr/bin/env bash
# Bring one VPT1 gateway up or down. The VPN is "on" while this gateway's controller runs.
#
#   vpt-gateway.sh up     CONFIG.toml [--wait SECONDS]   load the module, create vpn0, routes, the
#                                                        forwarding firewall and the MSS clamp, then
#                                                        start `vptctl run` in the background
#   vpt-gateway.sh down   CONFIG.toml                    stop the controller (the tunnel closes at
#                                                        once), remove vpn0, the routes and the MSS
#                                                        clamp, and leave a forward-nothing firewall
#   vpt-gateway.sh status CONFIG.toml                    one line: on / off
#
# Run as root on the gateway (a Pi, or inside a network namespace: `ip netns exec NS ...`).
#
# Environment:
#   VPTCTL        path to the vptctl binary                    (default: vptctl in PATH)
#   VPT1_MODULE   path to vpt1_foundation.ko, loaded only if the module is not loaded yet
#   VPT_RUN_DIR   pid file and controller log directory        (default: /run/vpt-gateway)
#
# Everything the script needs to know about the network comes from CONFIG.toml through
# `vptctl network`: the LAN interface and the peers' prefixes. There is no second copy to drift.
#
# Firewall: one small forwarding ruleset, and the only firewall the project has (the larger
# persistent perimeter was scrapped, docs/PERIMETER_REMOVAL.md). It forwards LAN <-> vpn0 for the
# peers' prefixes and nothing else, so a LAN host can never be forwarded to the underlay. The
# kernel's own ACL (the [[rule]] sections of the config) remains the authorization.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VPTCTL="${VPTCTL:-vptctl}"
RUN_DIR="${VPT_RUN_DIR:-/run/vpt-gateway}"
DEVICE=vpn0
TABLE=vpt_gateway

usage() {
    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

[ "$#" -ge 2 ] || usage
ACTION=$1
CONFIG=$2
shift 2
WAIT=0
while [ "$#" -gt 0 ]; do
    case "$1" in
    --wait) WAIT=${2:?--wait needs a number of seconds}; shift 2 ;;
    *) usage ;;
    esac
done

[ "$(id -u)" -eq 0 ] || { echo "vpt-gateway: run as root" >&2; exit 2; }
[ -r "${CONFIG}" ] || { echo "vpt-gateway: cannot read ${CONFIG}" >&2; exit 2; }
command -v "${VPTCTL}" >/dev/null 2>&1 || [ -x "${VPTCTL}" ] || { echo "vpt-gateway: vptctl not found (set VPTCTL)" >&2; exit 2; }

PIDFILE="${RUN_DIR}/controller.pid"
LOGFILE="${RUN_DIR}/controller.log"

# `vptctl network` validates the whole configuration and prints the LAN interface and the
# peers' prefixes; nothing else is read from the file here.
projection() {
    "${VPTCTL}" network "${CONFIG}"
}
LAN_IF=""
UNDERLAY_IF=""
PREFIXES=""
load_projection() {
    local json
    json=$(projection) || { echo "vpt-gateway: configuration rejected" >&2; exit 2; }
    LAN_IF=$(printf '%s' "${json}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["lan"]["interface"])')
    UNDERLAY_IF=$(printf '%s' "${json}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["underlay"]["interface"])')
    PREFIXES=$(printf '%s' "${json}" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(" ".join(p for peer in d["peers"] for p in peer["prefixes"]))')
}

controller_pid() {
    local pid
    [ -r "${PIDFILE}" ] || return 1
    pid=$(cat "${PIDFILE}")
    [ -n "${pid}" ] && kill -0 "${pid}" 2>/dev/null && grep -q vptctl "/proc/${pid}/cmdline" 2>/dev/null || return 1
    echo "${pid}"
}

stop_controller() {
    local pid i
    pid=$(controller_pid) || return 0
    kill -TERM "${pid}" 2>/dev/null || true
    for i in $(seq 1 50); do
        kill -0 "${pid}" 2>/dev/null || break
        sleep 0.1
    done
    kill -KILL "${pid}" 2>/dev/null || true
    rm -f "${PIDFILE}"
}

remove_network() {
    nft delete table inet "${TABLE}" 2>/dev/null || true
    nft delete table inet vpt_mss 2>/dev/null || true
    ip link del "${DEVICE}" 2>/dev/null || true # takes its routes with it
}

# The "off" state: the gateway forwards nothing, so a LAN host cannot reach the underlay or
# anything else through it. (Without this table the kernel would still forward LAN traffic to the
# cable subnet.)
install_isolation() {
    nft -f - <<EOF
table inet ${TABLE} {
	chain forward {
		type filter hook forward priority 0; policy drop;
	}
}
EOF
}

case "${ACTION}" in
status)
    if controller_pid >/dev/null; then echo "vpt-gateway: on"; else echo "vpt-gateway: off"; fi
    ;;
down)
    # The controller first: closing its socket closes admission at once (fail closed), then the
    # routes and firewall go. The kernel module stays loaded.
    stop_controller
    remove_network
    install_isolation
    echo "vpt-gateway: off"
    ;;
up)
    load_projection
    # Both interfaces must exist: the controller retries a missing one forever and only the log says so.
    for ifname in "${LAN_IF}" "${UNDERLAY_IF}"; do
        ip link show "${ifname}" >/dev/null 2>&1 || { echo "vpt-gateway: no interface ${ifname} (check lan_interface and underlay_interface in ${CONFIG}; \`ip -br link\` lists the real names)" >&2; exit 2; }
    done
    stop_controller
    remove_network
    mkdir -p "${RUN_DIR}"
    chmod 700 "${RUN_DIR}"
    : >"${LOGFILE}"

    if ! lsmod | grep -q '^vpt1_foundation '; then
        [ -n "${VPT1_MODULE:-}" ] || { echo "vpt-gateway: module not loaded and VPT1_MODULE not set" >&2; exit 2; }
        modprobe chacha20poly1305
        modprobe -q udp_tunnel || true
        insmod "${VPT1_MODULE}"
    fi

    ip link add "${DEVICE}" type vpt1_ay1
    ip link set "${DEVICE}" up
    sysctl -q -w net.ipv4.ip_forward=1
    for prefix in ${PREFIXES}; do
        ip route add "${prefix}" dev "${DEVICE}"
    done

    set_text="$(echo "${PREFIXES}" | sed 's/ /, /g')"
    nft -f - <<EOF
table inet ${TABLE} {
	chain forward {
		type filter hook forward priority 0; policy drop;
		iifname "${LAN_IF}" oifname "${DEVICE}" ip daddr { ${set_text} } accept
		iifname "${DEVICE}" oifname "${LAN_IF}" ip saddr { ${set_text} } accept
	}
}
EOF
    nft -f "${HERE}/mss-clamp.nft"

    nohup "${VPTCTL}" run "${CONFIG}" --device "${DEVICE}" >>"${LOGFILE}" 2>&1 </dev/null &
    echo $! >"${PIDFILE}"
    disown || true

    if [ "${WAIT}" -gt 0 ]; then
        for _ in $(seq 1 $((WAIT * 4))); do
            if grep -q 'established' "${LOGFILE}" 2>/dev/null; then
                echo "vpt-gateway: on (tunnel established)"
                exit 0
            fi
            controller_pid >/dev/null || { echo "vpt-gateway: controller exited; see ${LOGFILE}" >&2; tail -5 "${LOGFILE}" >&2; exit 1; }
            sleep 0.25
        done
        # The controller retries every failed session; its last reason is the useful part.
        reason=$(grep 'session ended' "${LOGFILE}" 2>/dev/null | tail -1 || true)
        if [ -n "${reason}" ]; then
            echo "vpt-gateway: controller running, tunnel not established after ${WAIT}s; last error: ${reason}" >&2
        else
            echo "vpt-gateway: controller running, tunnel not established after ${WAIT}s (peer down? see ${LOGFILE})" >&2
        fi
        exit 1
    fi
    echo "vpt-gateway: on (controller started; log ${LOGFILE})"
    ;;
*)
    usage
    ;;
esac
