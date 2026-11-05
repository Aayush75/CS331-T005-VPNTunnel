#!/usr/bin/env bash
# Shared helpers for the AN2 namespace fixtures (sourced, not executed).
#
# Rules this library enforces for every fixture:
#   * a fixture never touches a namespace it did not create: setup refuses to start
#     if any target namespace already exists;
#   * links are created directly inside their namespaces (ip -n A link add ... peer
#     name B netns C), so no fixture name ever exists in the root namespace;
#   * teardown removes only what this invocation recorded, verifies the removal and
#     reports failure instead of hiding it.

FX_NS=()

fx_require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "ERROR: $(basename "$0") must be run as root (or with sudo)." >&2
        exit 1
    fi
}

fx_ns_exists() {
    ip netns list | awk '{print $1}' | grep -qx -- "$1"
}

# Refuse to run over an existing fixture of the same run ID.
fx_require_free() {
    local ns
    for ns in "$@"; do
        if fx_ns_exists "$ns"; then
            echo "ERROR: namespace $ns already exists; refusing to touch another run's fixture." >&2
            exit 2
        fi
    done
}

fx_ns_add() {
    local ns
    for ns in "$@"; do
        ip netns add "$ns"
        FX_NS+=("$ns")
        ip -n "$ns" link set lo up
    done
}

# A namespace inherits net.ipv4.ip_forward from the host it is created on (hosted CI
# runners run with it on), so every non-gateway namespace turns forwarding off
# explicitly: forwarding exists only in the gateways (TEST_PLAN.md section 3).
fx_no_forwarding() {
    local ns
    for ns in "$@"; do
        ip netns exec "$ns" sysctl -q -w net.ipv4.ip_forward=0
    done
}

# fx_veth NS_A IF_A NS_B IF_B : one veth pair created in NS_A with its peer in NS_B.
fx_veth() {
    ip -n "$1" link add "$2" type veth peer name "$4" netns "$3"
}

# Delete every namespace named on the command line that exists, then verify.
# Returns non-zero if any namespace is still present afterwards.
fx_delete_ns() {
    local ns rc=0
    for ns in "$@"; do
        if fx_ns_exists "$ns"; then
            ip netns del "$ns" || true
        fi
        if fx_ns_exists "$ns"; then
            echo "ERROR: namespace $ns could not be removed" >&2
            rc=1
        fi
    done
    return "$rc"
}

# Remove the namespaces this invocation created, newest first.
fx_teardown_recorded() {
    local i rc=0
    for ((i = ${#FX_NS[@]} - 1; i >= 0; i--)); do
        fx_delete_ns "${FX_NS[$i]}" || rc=1
    done
    FX_NS=()
    return "$rc"
}
