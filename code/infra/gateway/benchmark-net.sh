#!/usr/bin/env bash
# Persistent dummy LAN address for locally generated Pi-to-Pi VPN measurements.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo 'Run as root' >&2; exit 2; }
side=$(cat /etc/vpntunnel/side)
case "$side" in
a) addr=10.77.1.1/24; hostaddr=10.77.1.10/24; remote=10.77.2.0/24 ;;
b) addr=10.77.2.1/24; hostaddr=10.77.2.10/24; remote=10.77.1.0/24 ;;
*) exit 2 ;;
esac
case "${1:-}" in
up)
    ip link show vptbench0 >/dev/null 2>&1 || ip link add vptbench0 type dummy
    ip address replace "$addr" dev vptbench0
    # Ordinary data excludes the configured gateway address; use a LAN-host alias.
    ip address replace "$hostaddr" dev vptbench0
    ip link set vptbench0 up
    if [ -z "$(ip route show exact "$remote")" ]; then
        ip route replace unreachable "$remote"
    fi
    ;;
down)
    ip link delete vptbench0 2>/dev/null || true
    ip route delete unreachable "$remote" 2>/dev/null || true
    ;;
*) echo 'usage: benchmark-net.sh up|down' >&2; exit 2 ;;
esac
