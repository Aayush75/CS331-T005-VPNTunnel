#!/usr/bin/env bash
# Boot the x86-64 reference image on macOS (Apple Silicon or Intel).
#
# macOS has no /dev/kvm, so run-vm.sh cannot be used. On Apple Silicon the
# guest runs under QEMU's TCG emulation: functionally the same guest kernel and
# userspace, several times slower, and never a performance baseline
# (TEAM_WORK_PLAN.md section 6). Intel Macs use HVF.
#
# Same disposable overlay, sizing and SSH forward as run-vm.sh. The serial
# console goes to a log file and QEMU runs in the background.
#
#   VPT1_IMAGE=/path/to/vpt1-ref-6.18-x86_64.qcow2 ./run-vm-macos.sh
#   ssh -p 2222 testuser@127.0.0.1
#   ./run-vm-macos.sh stop
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_IMG="${VPT1_IMAGE:-${SCRIPT_DIR}/vpt1-ref-6.18-x86_64.qcow2}"
STATE_DIR="$(cd "$(dirname "$BASE_IMG")" 2>/dev/null && pwd || true)"
if [ -z "$STATE_DIR" ]; then
    echo "ERROR: directory of '$BASE_IMG' does not exist."
    exit 1
fi
OVERLAY="${STATE_DIR}/vpt1-test-overlay.qcow2"
PIDFILE="${STATE_DIR}/vpt1-qemu.pid"
CONSOLE_LOG="${STATE_DIR}/vpt1-console.log"
EXPECTED_SHA256="f015bc123731185c5f6db9c3673a86a1c45cffd92ca39811a3dffad27497cc88"
SSH_PORT=2222

if [ "${1:-}" = "stop" ]; then
    pid="$(cat "$PIDFILE" 2>/dev/null || true)"
    if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
        echo "No running reference VM found."
        rm -f "$PIDFILE"
        exit 0
    fi
    # A stale pidfile can name a recycled PID: only signal a QEMU process.
    case "$(ps -p "$pid" -o comm= 2>/dev/null)" in
        *qemu-system-*) ;;
        *)
            echo "ERROR: pid $pid is not a QEMU process; leaving it alone and removing the stale pidfile."
            rm -f "$PIDFILE"
            exit 1
            ;;
    esac
    kill "$pid"
    # Wait for QEMU to exit so a following start never races it for the overlay.
    for _ in $(seq 1 60); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.5
    done
    if kill -0 "$pid" 2>/dev/null; then
        echo "ERROR: QEMU (pid $pid) did not exit within 30 s; the pidfile is kept."
        exit 1
    fi
    rm -f "$PIDFILE"
    echo "Stopped QEMU (pid $pid)."
    exit 0
fi

if [ "$(uname -s)" != "Darwin" ]; then
    echo "ERROR: this script is for macOS; use run-vm.sh on Linux."
    exit 1
fi

if [ ! -f "$BASE_IMG" ]; then
    echo "ERROR: Base image '$BASE_IMG' not found."
    echo "Download it (see MANIFEST.md) and set VPT1_IMAGE or place it in ${SCRIPT_DIR}."
    exit 1
fi

for tool in qemu-system-x86_64 qemu-img; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: $tool is not installed (brew install qemu)."
        exit 1
    fi
done

if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    echo "ERROR: reference VM already running (pid $(cat "$PIDFILE"))."
    exit 1
fi

if lsof -nP -iTCP:"$SSH_PORT" -sTCP:LISTEN >/dev/null 2>&1; then
    echo "ERROR: Host port $SSH_PORT is already in use."
    exit 1
fi

# Verify the master image once, before the first overlay is created.
if [ ! -f "$OVERLAY" ]; then
    echo "Verifying base image SHA-256..."
    actual="$(shasum -a 256 "$BASE_IMG" | awk '{print $1}')"
    if [ "$actual" != "$EXPECTED_SHA256" ]; then
        echo "ERROR: SHA-256 mismatch: got $actual, expected $EXPECTED_SHA256."
        exit 1
    fi
    echo "Creating disposable snapshot overlay..."
    qemu-img create -f qcow2 -b "$BASE_IMG" -F qcow2 "$OVERLAY"
fi

if [ "$(uname -m)" = "x86_64" ]; then
    ACCEL=(-accel hvf -cpu host)
else
    # Emulated x86-64; "max" exposes every feature TCG implements.
    ACCEL=(-accel "tcg,thread=multi" -cpu max)
fi

echo "Booting VPT1 Linux 6.18 Reference VM (${ACCEL[1]})..."
qemu-system-x86_64 \
    "${ACCEL[@]}" \
    -m 4096 \
    -smp 2 \
    -drive file="$OVERLAY",if=virtio \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:${SSH_PORT}-:22 \
    -device virtio-net-pci,netdev=net0 \
    -display none \
    -serial file:"$CONSOLE_LOG" \
    -pidfile "$PIDFILE" \
    -daemonize

echo "Started (pid $(cat "$PIDFILE")). Console log: $CONSOLE_LOG"
echo "Under TCG the first boot can take several minutes."
echo "SSH: ssh -p ${SSH_PORT} testuser@127.0.0.1"
echo "Stop: $0 stop   (changes persist in the overlay; reset-vm.sh discards them)"
