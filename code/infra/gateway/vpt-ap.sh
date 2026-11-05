#!/usr/bin/env bash
# Host the laptops' Wi-Fi network on a Pi (access point verified on a Raspberry Pi 5; see below): an access point (hostapd) and DHCP (dnsmasq)
# on the gateway's LAN interface, using the address in the gateway configuration.
#
#   vpt-ap.sh up      CONFIG.toml     start the access point and DHCP on the LAN interface
#   vpt-ap.sh down    CONFIG.toml     stop both and clear the LAN interface address
#   vpt-ap.sh render  CONFIG.toml     print the hostapd and dnsmasq settings, change nothing
#
# This is separate from vpt-gateway.sh (the VPN) so the network can be up with the VPN off, which is
# the "laptops cannot see each other" half of the demo. `up` was run on a Raspberry Pi 5 (hostapd
# reached AP-ENABLED, the interface was in AP mode, a phone joined, got a DHCP address and answered pings).
# `down` stopped both daemons and the Pi rejoined its campus Wi-Fi. `render` is covered by tests/unit/test_ap_script.py.
#
# Environment:
#   VPT_AP_SSID         network name                          (default: vpt-demo-<lan network>)
#   VPT_AP_PASSPHRASE   WPA2 passphrase, 8 to 63 characters   (required for up; never stored here)
#   VPT_AP_COUNTRY      two-letter regulatory country code    (required for up)
#   VPT_AP_CHANNEL      2.4 GHz channel                       (default: 6)
#   VPTCTL              path to the vptctl binary             (default: vptctl in PATH)
#   VPT_RUN_DIR         generated files and pid files         (default: /run/vpt-ap)
#
# The interface, its address and the DHCP range (.50 to .200 of the LAN /24, with the Pi as the
# default gateway and no DNS server handed out) come from the configuration through
# `vptctl network`. Needs: hostapd, dnsmasq, python3, iproute2. NetworkManager is told to leave
# the interface alone and wpa_supplicant is stopped while the access point runs (so the Pi's
# own Wi-Fi connection ends); `down` gives the interface back.
set -euo pipefail

VPTCTL="${VPTCTL:-vptctl}"
RUN_DIR="${VPT_AP_RUN_DIR:-${VPT_RUN_DIR:-/run/vpt-ap}}"
CHANNEL="${VPT_AP_CHANNEL:-6}"

usage() {
    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

[ "$#" -eq 2 ] || usage
ACTION=$1
CONFIG=$2
case "${ACTION}" in up | down | render) ;; *) usage ;; esac

[ -r "${CONFIG}" ] || { echo "vpt-ap: cannot read ${CONFIG}" >&2; exit 2; }
command -v "${VPTCTL}" >/dev/null 2>&1 || [ -x "${VPTCTL}" ] || { echo "vpt-ap: vptctl not found (set VPTCTL)" >&2; exit 2; }
if [ "${ACTION}" != render ] && [ "$(id -u)" -ne 0 ]; then
    echo "vpt-ap: run as root" >&2
    exit 2
fi

# One python call: interface, address, prefix length, network, DHCP range and mask.
fields=$("${VPTCTL}" network "${CONFIG}" | python3 -c '
import ipaddress, json, sys
lan = json.load(sys.stdin)["lan"]
iface = ipaddress.ip_interface(lan["address"])
net = iface.network
if net.prefixlen > 24:
    sys.exit("LAN network %s is too small for the DHCP range" % net)
first, last = net.network_address + 50, net.network_address + 200
print(lan["interface"], iface.ip, net.prefixlen, net, first, last, net.netmask)
') || { echo "vpt-ap: configuration rejected" >&2; exit 2; }
read -r LAN_IF LAN_IP LAN_PREFIX LAN_NET RANGE_FIRST RANGE_LAST NETMASK <<<"${fields}"

SSID="${VPT_AP_SSID:-vpt-demo-${LAN_NET%/*}}"
HOSTAPD_CONF="${RUN_DIR}/hostapd.conf"
HOSTAPD_PID="${RUN_DIR}/hostapd.pid"
DNSMASQ_PID="${RUN_DIR}/dnsmasq.pid"

# Never put the passphrase in a rendered listing.
render_hostapd() {
    local pass=$1
    cat <<EOF
interface=${LAN_IF}
driver=nl80211
ssid=${SSID}
country_code=${VPT_AP_COUNTRY:-XX}
hw_mode=g
channel=${CHANNEL}
wmm_enabled=1
auth_algs=1
wpa=2
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
wpa_passphrase=${pass}
EOF
}

render_dnsmasq_args() {
    cat <<EOF
--conf-file=/dev/null
--interface=${LAN_IF}
--bind-interfaces
--except-interface=lo
--port=0
--dhcp-range=${RANGE_FIRST},${RANGE_LAST},${NETMASK},12h
--dhcp-option=option:router,${LAN_IP}
--dhcp-option=option:dns-server
--dhcp-authoritative
--pid-file=${DNSMASQ_PID}
EOF
}

kill_pid() {
    local file=$1 pid
    [ -r "${file}" ] || return 0
    pid=$(cat "${file}")
    [ -n "${pid}" ] && kill -TERM "${pid}" 2>/dev/null || true
    rm -f "${file}"
}

case "${ACTION}" in
render)
    echo "# interface ${LAN_IF}, address ${LAN_IP}/${LAN_PREFIX}, network ${LAN_NET}"
    echo "# hostapd.conf"
    render_hostapd "<passphrase>"
    echo "# dnsmasq arguments"
    render_dnsmasq_args
    ;;
down)
    kill_pid "${HOSTAPD_PID}"
    kill_pid "${DNSMASQ_PID}"
    rm -f "${HOSTAPD_CONF}"
    ip addr flush dev "${LAN_IF}" 2>/dev/null || true
    command -v systemctl >/dev/null && systemctl start wpa_supplicant 2>/dev/null || true
    command -v nmcli >/dev/null && nmcli device set "${LAN_IF}" managed yes 2>/dev/null || true
    echo "vpt-ap: down"
    ;;
up)
    pass="${VPT_AP_PASSPHRASE:-}"
    [ "${#pass}" -ge 8 ] && [ "${#pass}" -le 63 ] || { echo "vpt-ap: set VPT_AP_PASSPHRASE (8 to 63 characters)" >&2; exit 2; }
    [[ "${VPT_AP_COUNTRY:-}" =~ ^[A-Z]{2}$ ]] || { echo "vpt-ap: set VPT_AP_COUNTRY to a two-letter code, e.g. IN" >&2; exit 2; }
    command -v hostapd >/dev/null && command -v dnsmasq >/dev/null || { echo "vpt-ap: hostapd and dnsmasq must be installed" >&2; exit 2; }
    ip link show "${LAN_IF}" >/dev/null 2>&1 || { echo "vpt-ap: no interface ${LAN_IF}" >&2; exit 2; }

    kill_pid "${HOSTAPD_PID}"
    kill_pid "${DNSMASQ_PID}"
    mkdir -p "${RUN_DIR}"
    chmod 700 "${RUN_DIR}"
    (umask 077 && render_hostapd "${pass}" >"${HOSTAPD_CONF}")

    command -v rfkill >/dev/null && rfkill unblock wifi || true
    command -v nmcli >/dev/null && nmcli device set "${LAN_IF}" managed no 2>/dev/null || true
    # The system wpa_supplicant (started for NetworkManager) flips the radio back to station mode
    # while hostapd runs, so the access point never carries a client. Stop it; `down` restarts it.
    command -v systemctl >/dev/null && systemctl stop wpa_supplicant 2>/dev/null || true
    ip addr flush dev "${LAN_IF}"
    ip addr add "${LAN_IP}/${LAN_PREFIX}" dev "${LAN_IF}"
    ip link set "${LAN_IF}" up

    hostapd -B -P "${HOSTAPD_PID}" "${HOSTAPD_CONF}"
    mapfile -t args < <(render_dnsmasq_args)
    dnsmasq "${args[@]}"
    echo "vpt-ap: up (SSID ${SSID} on ${LAN_IF}, ${LAN_IP}/${LAN_PREFIX}, DHCP ${RANGE_FIRST} to ${RANGE_LAST})"
    ;;
esac
