#!/usr/bin/env bash
# Run on a laptop to show the demo: can this laptop see the other one right now?
#
#   laptop-check.sh OTHER_LAPTOP_IP [TCP_PORT]
#
# Prints one line per probe and exits 0 only if everything probed is reachable:
#   ping 10.77.2.57: CAN SEE the other laptop
#   ping 10.77.2.57: CANNOT SEE the other laptop
# With a TCP port (for example a web server on the other laptop) it also tries to connect.
# Run it with the VPN off (the answer is CANNOT SEE), then on (CAN SEE), then off again.
set -u

target=${1:-}
port=${2:-}
[ -n "${target}" ] || { echo "usage: $0 OTHER_LAPTOP_IP [TCP_PORT]" >&2; exit 2; }

status=0
if ping -c 2 -W 1 "${target}" >/dev/null 2>&1; then
    echo "ping ${target}: CAN SEE the other laptop"
else
    echo "ping ${target}: CANNOT SEE the other laptop"
    status=1
fi
if [ -n "${port}" ]; then
    if timeout 3 bash -c "exec 3<>/dev/tcp/${target}/${port}" 2>/dev/null; then
        echo "tcp ${target}:${port}: CAN SEE the other laptop"
    else
        echo "tcp ${target}:${port}: CANNOT SEE the other laptop"
        status=1
    fi
fi
exit "${status}"
