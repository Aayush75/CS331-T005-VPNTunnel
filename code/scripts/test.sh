#!/usr/bin/env bash
# Run the full test suite: the kernel unit modules, the end-to-end network-namespace
# tests, the controller's own Rust tests, and the AY1 smoke test. This is the single
# command a TA runs to reproduce the functional results.
#
# Must run as root on a DISPOSABLE Linux 6.18.y machine (the reference VM): it loads and
# unloads kernel modules and creates/destroys network namespaces, which is global to the host.
#
#   sudo ./scripts/test.sh
#
# What it covers (see docs/testing.md for the full list):
#   - every kernel unit-test module (parser, sequence, AEAD, ACL, flow, zone, generation,
#     fragment, IP, gateway, policy, witness)
#   - the Generic Netlink end-to-end test, the two-gateway data path, re-key, liveness,
#     activation, the fragment / ICMP / MTU live adapter, multi-peer isolation,
#     configuration reload and validation, runtime accounting and the demo reachability test
#   - a kmemleak scan at unload when the kernel supports it
set -uo pipefail
. "$(dirname "$0")/common.sh"

require_root
require_linux_618

VPTCTL="$(build_vptctl)" || exit 1
export VPTCTL

say "running the full kernel + namespace suite (debug and release builds)"
VPT1_DISPOSABLE_VM=yes VPTCTL="${VPTCTL}" bash "${CODE_DIR}/tools/kernel_vm_suite.sh"
rc=$?

echo
if [ "${rc}" -eq 0 ]; then
    say "controller unit and integration tests (Rust)"
    ( cd "${CODE_DIR}/control" && cargo test --release --locked ) || rc=1
fi

echo
if [ "${rc}" -eq 0 ]; then
    say "ALL TESTS PASSED"
else
    say "TESTS FAILED (exit ${rc})"
fi
exit "${rc}"
