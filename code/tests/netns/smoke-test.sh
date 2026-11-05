#!/usr/bin/env bash
# AN2: Automated smoke test for network namespaces, veth, nftables, and conntrack

set -euo pipefail

# --------------------------------------------------
# 1. Check privileges and required commands
# --------------------------------------------------

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: This test must be run as root or with sudo."
    exit 1
fi

for cmd in ip nft conntrack ping modprobe awk grep; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "ERROR: Required command '$cmd' is not installed."
        exit 1
    fi
done

# Load nftables conntrack expression module.
echo "==> Loading nft_ct kernel module..."
modprobe nft_ct

# --------------------------------------------------
# 2. Define namespaces and cleanup
# --------------------------------------------------

NS_A="vpt_smoke_a_$$"
NS_B="vpt_smoke_b_$$"
VETH_A="vpa$$"
VETH_B="vpb$$"
veth_created=false
ns_a_created=false
ns_b_created=false

cleanup() {
    local rc=$?
    local ns link
    trap - EXIT
    echo "==> Cleaning up test resources..."

    # A link may already have moved into a namespace or disappeared with its peer.
    if "$veth_created"; then
        for link in "$VETH_A" "$VETH_B"; do
            if ip link show dev "$link" >/dev/null 2>&1 &&
               ! ip link del "$link"; then
                echo "ERROR: Failed to remove test link $link" >&2
                rc=1
            fi
        done
    fi

    for ns in "$NS_A" "$NS_B"; do
        if [[ "$ns" == "$NS_A" ]] && ! "$ns_a_created"; then continue; fi
        if [[ "$ns" == "$NS_B" ]] && ! "$ns_b_created"; then continue; fi
        if ! ip netns del "$ns"; then
            echo "ERROR: Failed to remove test namespace $ns" >&2
            rc=1
        fi
    done
    if (( rc == 0 )); then
        echo "[PASS] AN2 network namespace smoke test and cleanup succeeded!"
    fi
    exit "$rc"
}

trap cleanup EXIT

# --------------------------------------------------
# 3. Create network namespaces
# --------------------------------------------------

echo "==> [1/4] Creating namespaces..."

ip netns add "$NS_A"
ns_a_created=true
ip netns add "$NS_B"
ns_b_created=true

# --------------------------------------------------
# 4. Create veth pair and configure IPv4
# --------------------------------------------------

echo "==> [2/4] Setting up veth pair and IP addresses..."

ip link add "$VETH_A" type veth peer name "$VETH_B"
veth_created=true

ip link set "$VETH_A" netns "$NS_A"
ip link set "$VETH_B" netns "$NS_B"

# Configure namespace A.
ip netns exec "$NS_A" ip link set lo up
ip netns exec "$NS_A" ip addr add 10.99.1.1/24 dev "$VETH_A"
ip netns exec "$NS_A" ip link set "$VETH_A" up

# Configure namespace B.
ip netns exec "$NS_B" ip link set lo up
ip netns exec "$NS_B" ip addr add 10.99.1.2/24 dev "$VETH_B"
ip netns exec "$NS_B" ip link set "$VETH_B" up

# --------------------------------------------------
# 5. Configure nftables stateful filtering
# --------------------------------------------------

echo "==> [3/4] Installing nftables stateful filter..."

ip netns exec "$NS_B" nft add table inet filter

ip netns exec "$NS_B" nft \
    'add chain inet filter input { type filter hook input priority 0 ; policy accept ; }'

# Match incoming new requests and established replies/flows
ip netns exec "$NS_B" nft \
    'add rule inet filter input ct state { new, established } counter accept'

# --------------------------------------------------
# 6. Generate traffic and validate connectivity
# --------------------------------------------------

echo "==> [4/4] Generating traffic..."

# A pings B
ip netns exec "$NS_A" ping -c 3 -W 2 10.99.1.2 > /dev/null
# B pings A to ensure bidirectional established flow tracking
ip netns exec "$NS_B" ping -c 2 -W 2 10.99.1.1 > /dev/null

echo "    -> Verified: IPv4 bidirectional connectivity between namespaces."

# --------------------------------------------------
# 7. Verify conntrack entries
# --------------------------------------------------

FLOW_COUNT=$(
    ip netns exec "$NS_B" conntrack -L 2>/dev/null |
    grep -c "10.99.1.1" || true
)

if [ "$FLOW_COUNT" -lt 1 ]; then
    echo "ASSERTION FAILED: conntrack did not record the flow!"
    exit 1
fi

echo "    -> Verified: conntrack recorded ${FLOW_COUNT} flow entry/entries."

# --------------------------------------------------
# 8. Verify nftables packet counter
# --------------------------------------------------

PACKET_COUNT=$(
    ip netns exec "$NS_B" nft -nn list chain inet filter input |
    awk '/ct state/ && !found {
        for (i=1; i<=NF; i++)
            if ($i=="packets") {
                print $(i+1)
                found = 1
                break
            }
    }'
)

if [ -z "$PACKET_COUNT" ] || [ "$PACKET_COUNT" -lt 1 ]; then
    echo "ASSERTION FAILED: nftables stateful counter did not increment!"
    exit 1
fi

echo "    -> Verified: nftables counted ${PACKET_COUNT} stateful packets."

# --------------------------------------------------
# 9. Final result
# --------------------------------------------------

echo
echo "=================================================="
echo "AN2 network namespace checks succeeded; cleanup follows."
echo "       - Network namespaces"
echo "       - Virtual Ethernet connectivity"
echo "       - IPv4 communication"
echo "       - nftables established-state filtering"
echo "       - Conntrack flow tracking"
echo "       - Cleanup will be checked before reporting success"
echo "=================================================="
