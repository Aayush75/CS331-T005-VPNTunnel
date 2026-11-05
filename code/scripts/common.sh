#!/usr/bin/env bash
# Shared helpers for the VPTunnel reproduction scripts.
# Source this from the other scripts; it defines CODE_DIR and a few guards.
set -uo pipefail

# The repository's code/ directory, whatever the caller's working directory is.
CODE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export CODE_DIR

# The network tools the fixtures use (ip, sysctl, nft, ethtool) live in /usr/sbin and /sbin,
# which a non-login sudo shell often drops from PATH. Ensure they are reachable.
case ":${PATH}:" in
    *:/usr/sbin:*) ;;
    *) PATH="/usr/sbin:/sbin:${PATH}"; export PATH ;;
esac

say()  { printf '==> %s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

require_root() {
    [ "$(id -u)" -eq 0 ] || die "run as root (the kernel module and network namespaces need it)"
}

require_linux_618() {
    case "$(uname -r)" in
    6.18.*) ;;
    *) die "this needs a Linux 6.18.y kernel; found $(uname -r). Use the reference VM (see docs/reproducing.md)." ;;
    esac
}

# Build vptctl (release) if it is not already built; print its path.
build_vptctl() {
    local bin="${CODE_DIR}/control/target/release/vptctl"
    if [ ! -x "${bin}" ]; then
        say "building vptctl (release)"
        ( cd "${CODE_DIR}/control" && cargo build --release --locked ) || die "vptctl build failed"
    fi
    printf '%s\n' "${bin}"
}

# Build the kernel module (release) if it is not already built; print its path.
build_module() {
    local ko="${CODE_DIR}/kernel/vpt1_foundation.ko"
    if [ ! -f "${ko}" ]; then
        say "building the kernel module"
        make -C "${CODE_DIR}/kernel" >/dev/null || die "kernel module build failed"
    fi
    printf '%s\n' "${ko}"
}
