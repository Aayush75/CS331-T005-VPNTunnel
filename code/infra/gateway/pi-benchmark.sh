#!/usr/bin/env bash
# Installed as vpt-bench on each prepared Pi. No performance traffic is generated.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo 'Use sudo vpt-bench ...' >&2; exit 2; }
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
export VPTCTL=/usr/local/bin/vptctl
export VPT1_MODULE=/usr/local/lib/vpt1/vpt1_foundation.ko
export VPT_RUN_DIR=/run/vpt-benchmark
cfg=/etc/vpntunnel/benchmark.toml
side=$(cat /etc/vpntunnel/side)
case "$side" in a) remote=10.77.2.0/24 ;; b) remote=10.77.1.0/24 ;; *) exit 2 ;; esac
gateway=/usr/local/lib/vpt1/vpt-gateway.sh
case "${1:-}" in
up)
    systemctl start vpt-benchmark-net.service
    ip route delete unreachable "$remote" 2>/dev/null || true
    if [ "$side" = a ]; then exec "$gateway" up "$cfg" --wait 20
    else exec "$gateway" up "$cfg"; fi
    ;;
down)
    "$gateway" down "$cfg"
    ip route replace unreachable "$remote"
    ;;
net-up) systemctl start vpt-benchmark-net.service ;;
net-down) "$gateway" down "$cfg"; systemctl stop vpt-benchmark-net.service ;;
status)
    "$gateway" status "$cfg"
    ip -br address show eth0
    ip -br address show vptbench0
    ip link show vpn0 2>/dev/null || true
    ;;
ready)
    "$VPTCTL" check-config "$cfg" --with-key
    uname -a
    getconf PAGESIZE
    ethtool eth0 | sed -n '/Speed:/p;/Duplex:/p;/Link detected:/p'
    vcgencmd measure_temp
    vcgencmd get_throttled
    timedatectl show -p NTPSynchronized
    ;;
*) echo 'usage: sudo vpt-bench {up|down|status|ready|net-up|net-down}' >&2; exit 2 ;;
esac
