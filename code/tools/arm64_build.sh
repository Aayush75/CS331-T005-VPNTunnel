#!/usr/bin/env bash
# AY1/AY3 ARM64 + Raspberry Pi 5 (16 KiB page) build lane (docs/PLATFORM_PI5.md).
#
# This is the BUILD half of the platform lane assigned to Aayush: it cross-compiles the
# project modules for ARM64 against a prepared ARM64 kernel build tree, runs the AY1
# build-prerequisite inventory against that tree, and audits the source for 4096-byte
# page-shift assumptions that break on the Pi's 16 KiB pages. It does NOT load a module and
# does NOT run the namespace suite: a cross-build host has no ARM64 runtime. Runtime
# qualification stays the Pi/ARM64-VM runtime lane. A clean result here is a BUILD result
# only; it never prints a runtime PASS.
#
# Usage:
#   KDIR=/path/to/arm64-6.18-build \
#   CROSS_COMPILE=aarch64-linux-gnu- \
#   [CONFIG=/path/to/arm64/.config] [PAGE_SHIFT=14] \
#   tools/arm64_build.sh
#
# KDIR must be a prepared ARM64 6.18.y build tree (headers + Module.symvers), e.g. the Pi's
# /lib/modules/<release>/build copied to the build host, or a cross-built kernel output tree.
# PAGE_SHIFT defaults to 14 (16 KiB, the rpi-2712 build); pass 12 to check a 4 KiB ARM64 VM.
set -uo pipefail

ARCH=arm64
: "${KDIR:?set KDIR to a prepared ARM64 6.18 kernel build tree}"
: "${CROSS_COMPILE:?set CROSS_COMPILE, e.g. aarch64-linux-gnu-}"
PAGE_SHIFT="${PAGE_SHIFT:-14}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"
FAIL=0
fail() { echo "FAIL: $*" >&2; FAIL=1; }
note() { echo "  $*"; }

# 0. Toolchain present.
if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    echo "ERROR: ${CROSS_COMPILE}gcc not found; install the aarch64 cross toolchain." >&2
    exit 2
fi
case "$(cat "${KDIR}/include/config/kernel.release" 2>/dev/null)" in
6.18.*) ;;
*) echo "ERROR: KDIR is not a 6.18.y tree (release=$(cat "${KDIR}/include/config/kernel.release" 2>/dev/null))." >&2; exit 2 ;;
esac

echo "######## ARM64 cross-build (ARCH=${ARCH}, CROSS_COMPILE=${CROSS_COMPILE}, page 2^${PAGE_SHIFT})"

# 1. Build the foundation and every test module for ARM64, debug and release, W=1.
for kind in release debug; do
    make -C kernel clean >/dev/null 2>&1
    dbg=""
    [ "${kind}" = debug ] && dbg="VPT1_DEBUG=1"
    out=$(make -C "${KDIR}" M="${ROOT}/kernel" ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" \
               W=1 ${dbg} modules 2>&1)
    if [ $? -ne 0 ]; then
        echo "${out}" | tail -40 >&2
        fail "${kind} ARM64 build failed"
        continue
    fi
    if echo "${out}" | grep -E "error:|warning:"; then
        fail "${kind} ARM64 build has warnings or errors"
    fi
    [ -f kernel/vpt1_foundation.ko ] || fail "${kind} ARM64 build produced no module"
    # modpost sanity: the module must declare an aarch64/ARM64 vermagic. If objdump produced a
    # .modinfo at all and it is NOT aarch64, the cross-build targeted the wrong arch -- fail.
    if command -v "${CROSS_COMPILE}objdump" >/dev/null 2>&1 && [ -f kernel/vpt1_foundation.ko ]; then
        vm=$(${CROSS_COMPILE}objdump -s -j .modinfo kernel/vpt1_foundation.ko 2>/dev/null | tr -d '\0')
        if [ -n "${vm}" ] && echo "${vm}" | grep -q "vermagic="; then
            echo "${vm}" | grep -Eq "aarch64|arm64|ARM64" || \
                fail "${kind} module vermagic is not aarch64/arm64 (wrong ARCH/CROSS_COMPILE?)"
        else
            fail "${kind} module has no readable .modinfo vermagic to check the target arch"
        fi
    fi
    note "PASS ${kind} ARM64 modules built, W=1 clean"
done

# 2. AY1 build-prerequisite inventory for the ARM64 tree (reuses the shared tool).
PREFLIGHT=ran
if [ -f tools/ay1_preflight.py ]; then
    cfg="${CONFIG:-${KDIR}/.config}"
    if [ -r "${cfg}" ]; then
        mkdir -p artifacts/arm64
        if python3 tools/ay1_preflight.py --kdir "${KDIR}" --config "${cfg}" \
                   --output artifacts/arm64/ay1-preflight-arm64.json; then
            note "PASS AY1 ARM64 preflight (artifacts/arm64/ay1-preflight-arm64.json)"
        else
            fail "AY1 ARM64 preflight failed; see artifacts/arm64/ay1-preflight-arm64.json"
        fi
    else
        PREFLIGHT=skipped
        note "SKIP AY1 preflight: no readable CONFIG (pass CONFIG=/path/to/.config)"
    fi
else
    PREFLIGHT=skipped
fi

# 3. Page-size assumption audit (docs/PLATFORM_PI5.md step 3). A hard-coded 4096 or a shift of
#    12 in allocation/accounting/skb/scatterlist code is a 16 KiB hazard; PAGE_SIZE/PAGE_SHIFT
#    helpers are not. This is a lint, not a proof: it flags, a human clears.
echo "######## 16 KiB page-assumption audit"
# Match on the CODE only, never the "file:line:" prefix: a line number containing 1024 must not
# hide a hit, and a literal 4096 (including 4096 * 1024) must be reported. Exclusions apply to the
# code text: our own VPT1_ constants, comment lines, and a byte/size literal written with an
# explicit multiplier or unit suffix are not page-shift hazards.
hits=$(grep -RnE "\b4096\b|<< *12\b|PAGE_SHIFT *== *12|(^|[^_])\bPAGE_SIZE\b *== *4096" \
         kernel/ tests/unit/ 2>/dev/null \
       | awk -F':' 'BEGIN{OFS=":"} { code=$0; sub(/^[^:]*:[0-9]+:/,"",code);
             if (code ~ /VPT1_[A-Z0-9_]*(4096|PAGE)/) next;        # our own named constants
             if (code ~ /^[[:space:]]*(\/\*|\*|\/\/)/) next;       # comment lines
             print }' || true)
if [ -n "${hits}" ]; then
    echo "${hits}"
    note "REVIEW: the lines above mention a literal page size; confirm each uses a page helper"
    note "        or is page-size-independent before claiming the 16 KiB lane. Not an automatic fail."
else
    note "PASS no literal 4096/<<12 page assumptions in kernel/ or tests/unit/"
fi
note "declared target page size for this run: $((1 << PAGE_SHIFT)) bytes"
[ "${PREFLIGHT}" = skipped ] && note "WARNING: AY1 preflight was SKIPPED (no CONFIG) -- inventory not verified"

echo ""
if [ "${FAIL}" -eq 0 ]; then
    echo "ARM64_BUILD PASS (build only$([ "${PREFLIGHT}" = ran ] && echo " + inventory"); runtime is the Pi/ARM64-VM lane; preflight=${PREFLIGHT})"
else
    echo "ARM64_BUILD FAIL"
fi
exit "${FAIL}"
