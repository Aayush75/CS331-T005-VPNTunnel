#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_IMG="${SCRIPT_DIR}/vpt1-ref-6.18-x86_64.qcow2"
OVERLAY="${SCRIPT_DIR}/vpt1-test-overlay.qcow2"

if [ ! -f "$BASE_IMG" ]; then
    echo "ERROR: Base image '$BASE_IMG' not found."
    echo "Please download the reference image and place it in ${SCRIPT_DIR}."
    exit 1
fi

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "ERROR: qemu-system-x86_64 is not installed."
    exit 1
fi

if ! command -v qemu-img >/dev/null 2>&1; then
    echo "ERROR: qemu-img is not installed."
    exit 1
fi

if [ ! -e /dev/kvm ]; then
    echo "ERROR: /dev/kvm is unavailable. KVM acceleration cannot be used."
    exit 1
fi

if command -v ss >/dev/null 2>&1 &&
   ss -ltn | grep -q ':2222 '; then
    echo "ERROR: Host port 2222 is already in use."
    exit 1
fi

# Create a disposable copy-on-write overlay if it does not exist.
if [ ! -f "$OVERLAY" ]; then
    echo "Creating disposable snapshot overlay..."
    qemu-img create -f qcow2 -b "$BASE_IMG" -F qcow2 "$OVERLAY"
fi

echo "Booting VPT1 Linux 6.18 Reference VM..."
echo "Console login: testuser (credentials provided separately)"
echo "SSH: ssh -p 2222 testuser@localhost"
echo "Changes persist in the test overlay."
echo "To reset, run reset-vm.sh after shutting down the VM."
echo "Exit QEMU: Ctrl+A, then X"
echo "---------------------------------------------------"

qemu-system-x86_64 \
    -enable-kvm \
    -m 4096 \
    -smp 2 \
    -cpu host \
    -drive file="$OVERLAY",if=virtio \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 \
    -device virtio-net-pci,netdev=net0 \
    -nographic
