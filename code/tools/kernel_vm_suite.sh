#!/usr/bin/env bash
# Run every kernel-side test of this repository inside a disposable Linux 6.18 VM.
#
# GitHub-hosted runners have no 6.18 headers and cannot load modules, so this suite
# cannot run in the hosted CI lane. It is the single command a self-hosted or
# disposable-VM job (AN3) runs, and the one to run by hand on the reference VM:
#
#   sudo VPT1_DISPOSABLE_VM=yes VPTCTL=/path/to/vptctl tools/kernel_vm_suite.sh
#
# VPT1_PREBUILT=yes runs only the debug kind, on modules already built in kernel/ (against a kernel that has
# no module headers, such as the KASAN/lockdep/kmemleak kernel of docs/QUALITY_CHECKS.md). It does not build.
# When the kernel has kmemleak, the suite scans for leaks at the end and fails on any report.
#
# VPTCTL is optional: with a vptctl binary (cargo build --release in control/) the suite also
# runs the controller end to end; without it that step is reported as SKIP.
#
# It builds a release and a debug (VPT1_DEBUG=1) tree, loads each unit-test module and
# requires its PASS line, runs the end-to-end Generic Netlink test and the two-gateway
# data-path test against both builds, the AY1 smoke script, checks the kernel log for faults, and fails on any leak
# report at unload. Module load and unload are guest-global: never run it on a developer
# machine.
set -uo pipefail

if [ "${VPT1_DISPOSABLE_VM:-}" != "yes" ]; then
    echo "ERROR: set VPT1_DISPOSABLE_VM=yes to confirm this is a disposable VM." >&2
    exit 2
fi
if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: run as root." >&2
    exit 2
fi
case "$(uname -r)" in
6.18.*) ;;
*) echo "ERROR: kernel $(uname -r) is not 6.18.y." >&2; exit 2 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"
FAIL=0
fail() { echo "FAIL: $*" >&2; FAIL=1; }

if [ "${VPT1_PREBUILT:-}" = yes ]; then
    : # Prebuilt debug kernels have chacha20poly1305 and the UDP tunnel code built in.
else
    modprobe chacha20poly1305 || { echo "ERROR: chacha20poly1305 provider unavailable." >&2; exit 2; }
    modprobe -q udp_tunnel || true
fi

# A hung module init cannot be unloaded and needs a reboot, so every insmod is bounded
# and a timeout is reported loudly instead of waiting forever.
load() { # MODULE.ko
    timeout 120 insmod "$1"
    local rc=$?
    if [ "${rc}" -eq 124 ]; then
        echo "FATAL: insmod $1 timed out; the guest needs a reboot before further runs." >&2
        exit 3
    fi
    return "${rc}"
}

fault_markers() {
    dmesg | grep -E "BUG:|WARNING:|Oops|KASAN|UBSAN|refcount_t|lockdep|possible (circular|recursive)" || true
}

unit_modules() { # BUILD_KIND
    local mods="parser_test sequence_test aead_test acl_test flow_test zone_test generation_test fragment_test ip_test"
    [ "$1" = debug ] && mods="${mods} gwtest policy_test"
    local m line
    for m in ${mods}; do
        dmesg -C
        if ! load "kernel/vpt1_${m}.ko"; then
            dmesg
            fail "${m} failed to load (${1})"
            continue
        fi
        line=$(dmesg | grep -E "VPT1_[A-Z]+_TEST: " | tail -1)
        case "${line}" in
        *" PASS") echo "  PASS ${m}: ${line##*VPT1_}" ;;
        *) fail "${m} did not report PASS (${line:-no summary line})" ;;
        esac
        rmmod "vpt1_${m}" || fail "${m} did not unload (${1})"
        [ -n "$(fault_markers)" ] && fail "kernel fault marker after ${m}"
    done
}

foundation_e2e() { # BUILD_KIND
    dmesg -C
    load kernel/vpt1_foundation.ko || { fail "foundation failed to load"; return; }
    if python3 tests/netns/ay2_genl.py; then
        echo "  PASS ay2_genl (${1})"
    else
        fail "ay2_genl failed (${1})"
    fi
    if python3 tests/netns/ay2_datapath.py; then
        echo "  PASS ay2_datapath (${1})"
    else
        fail "ay2_datapath failed (${1})"
    fi
    if python3 tests/netns/ay4_liveness.py; then
        echo "  PASS ay4_liveness (${1})"
    else
        fail "ay4_liveness failed (${1})"
    fi
    mkdir -p artifacts/hr4
    if python3 tests/netns/hr4_ip.py --output "artifacts/hr4/live-${1}.json"; then
        echo "  PASS hr4_ip (${1})"
    else
        fail "hr4_ip failed (${1})"
    fi
    if python3 tests/netns/hr4_mtu.py --output "artifacts/hr4/mtu-${1}.json"; then
        echo "  PASS hr4_mtu (${1})"
    else
        fail "hr4_mtu failed (${1})"
    fi
    if python3 tests/netns/hr3_live.py; then
        echo "  PASS hr3_live (${1})"
    else
        fail "hr3_live failed (${1})"
    fi
    mkdir -p artifacts/hr5
    if python3 tests/netns/hr5_multi.py --output "artifacts/hr5/multi-${1}.json"; then
        echo "  PASS hr5_multi (${1})"
    else
        fail "hr5_multi failed (${1})"
    fi
    if [ "$1" = debug ]; then
        if python3 tests/netns/hr5_final_cut.py --output artifacts/hr5/final-cut.json; then
            echo "  PASS hr5_final_cut (${1})"
        else
            fail "hr5_final_cut failed (${1})"
        fi
        if python3 tests/netns/hr5_rx_deadline.py --output artifacts/hr5/rx-deadline.json; then
            echo "  PASS hr5_rx_deadline (${1})"
        else
            fail "hr5_rx_deadline failed (${1})"
        fi
        if python3 tests/netns/hr5_control_cut.py --output artifacts/hr5/control-cut.json; then
            echo "  PASS hr5_control_cut (${1})"
        else
            fail "hr5_control_cut failed (${1})"
        fi
    elif [ -e /sys/module/vpt1_foundation/parameters/tx_test_delay_us ] ||
         [ -e /sys/module/vpt1_foundation/parameters/tx_test_seen ] ||
         [ -e /sys/module/vpt1_foundation/parameters/rx_test_delay_us ] ||
         [ -e /sys/module/vpt1_foundation/parameters/rx_test_seen ] ||
         [ -e /sys/module/vpt1_foundation/parameters/control_test_delay_us ] ||
         [ -e /sys/module/vpt1_foundation/parameters/control_test_seen ] ||
         [ -e /sys/module/vpt1_foundation/parameters/control_test_netns ]; then
        fail "debug packet test parameters present in release"
    else
        echo "  PASS no debug packet test parameters (release)"
    fi
    if [ -n "${VPTCTL:-}" ]; then
        if python3 tests/netns/ay2_controller.py "${VPTCTL}"; then
            echo "  PASS ay2_controller (${1})"
        else
            fail "ay2_controller failed (${1})"
        fi
        if python3 tests/netns/ay4_usage_rekey.py "${VPTCTL}"; then
            echo "  PASS ay4_usage_rekey (${1})"
        else
            fail "ay4_usage_rekey failed (${1})"
        fi
        if python3 tests/netns/ay4_activation.py "${VPTCTL}"; then
            echo "  PASS ay4_activation (${1})"
        else
            fail "ay4_activation failed (${1})"
        fi
        if python3 tests/netns/ay4_silence.py "${VPTCTL}"; then
            echo "  PASS ay4_silence (${1})"
        else
            fail "ay4_silence failed (${1})"
        fi
        if python3 tests/netns/ay4_soak.py "${VPTCTL}"; then
            echo "  PASS ay4_soak (${1})"
        else
            fail "ay4_soak failed (${1})"
        fi
        # Default seed with 1200 configurations, then two more seeds at 600 each: a pass is "no
        # disagreement in these draws", so more draws cost little. The run also checks the fixed
        # must-accept list, which is the same for every seed.
        for corpus_args in "" "--seed 1 --count 600" "--seed 2 --count 600"; do
            # shellcheck disable=SC2086
            if python3 tests/netns/rj4_validate_corpus.py "${VPTCTL}" ${corpus_args}; then
                echo "  PASS rj4_validate_corpus ${corpus_args:-default} (${1})"
            else
                fail "rj4_validate_corpus ${corpus_args:-default} failed (${1})"
            fi
        done
        if python3 tests/netns/rj4_reload.py "${VPTCTL}"; then
            echo "  PASS rj4_reload (${1})"
        else
            fail "rj4_reload failed (${1})"
        fi
        if python3 tests/netns/ay5_runtime.py "${VPTCTL}"; then
            echo "  PASS ay5_runtime (${1})"
        else
            fail "ay5_runtime failed (${1})"
        fi
        if python3 tests/netns/demo_reachability.py "${VPTCTL}"; then
            echo "  PASS demo_reachability (${1})"
        else
            fail "demo_reachability failed (${1})"
        fi
    else
        echo "  SKIP ay2_controller: set VPTCTL to a vptctl binary (cargo build --release in control/)"
    fi
    rmmod vpt1_foundation || fail "foundation did not unload"
    local report
    report=$(dmesg | grep "vpt1_ay1: unloaded" | tail -1)
    case "${report}" in
    *"gw=0 peer=0 cfg=0 gen=0"*) echo "  PASS no leaked objects (${1})" ;;
    *) fail "leak report at unload: ${report:-missing}" ;;
    esac
    [ -n "$(fault_markers)" ] && fail "kernel fault marker after foundation (${1})"
}

kinds="debug release"
[ "${VPT1_PREBUILT:-}" = yes ] && kinds="debug"
for kind in ${kinds}; do
    echo "######## ${kind} build"
    if [ "${VPT1_PREBUILT:-}" = yes ]; then
        echo "  using the prebuilt modules in kernel/"
    else
        make -C kernel clean >/dev/null 2>&1
        mkdir -p artifacts/kernel-vm
        build_log="artifacts/kernel-vm/build-${kind}.log"
        build_flags=()
        [ "${kind}" = debug ] && build_flags+=(VPT1_DEBUG=1)
        if ! make -C kernel W=1 "${build_flags[@]}" >"${build_log}" 2>&1; then
            cat "${build_log}"
            fail "${kind} build failed"
            continue
        fi
        if grep -Ei '(^|[[:space:]])(fatal error|error|warning):' "${build_log}"; then
            fail "${kind} build has compiler diagnostics"
        fi
    fi
    [ -f kernel/vpt1_foundation.ko ] || { fail "${kind} build produced no module"; continue; }
    unit_modules "${kind}"
    foundation_e2e "${kind}"
done

if [ "${VPT1_PREBUILT:-}" = yes ]; then
    echo "######## AY1 smoke skipped (prebuilt debug modules only)"
else
echo "######## AY1 smoke (release build)"
if env PATH=/usr/sbin:/usr/bin:/sbin:/bin AY1_DISPOSABLE_VM=yes timeout 120 sh tests/netns/ay1_smoke.sh kernel | tail -1 | grep -q AY1_SMOKE_PASS; then
    echo "  PASS ay1_smoke"
else
    fail "ay1_smoke failed"
fi
fi

if [ -w /sys/kernel/debug/kmemleak ]; then
    echo "######## kmemleak"
    sleep 6
    echo scan > /sys/kernel/debug/kmemleak
    sleep 2
    echo scan > /sys/kernel/debug/kmemleak
    if [ -s /sys/kernel/debug/kmemleak ]; then
        fail "kmemleak reported unreferenced objects:"
        head -60 /sys/kernel/debug/kmemleak >&2
    else
        echo "  PASS kmemleak: no unreferenced objects"
    fi
fi

[ "$(lsmod | grep -c '^vpt1')" -eq 0 ] || fail "project modules still loaded"
if [ "${FAIL}" -eq 0 ]; then
    echo "KERNEL_VM_SUITE PASS"
else
    echo "KERNEL_VM_SUITE FAIL"
fi
exit "${FAIL}"
