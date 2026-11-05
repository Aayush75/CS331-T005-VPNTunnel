#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OVERLAY="${SCRIPT_DIR}/vpt1-test-overlay.qcow2"

if [ -f "$OVERLAY" ]; then
    read -rp "WARNING: This will permanently delete your test overlay and discard all uncommitted VM changes. Continue? [y/N] " confirm
    if [[ "$confirm" =~ ^[Yy]$ ]]; then
        rm -f "$OVERLAY"
        echo "Disposable test overlay removed. A fresh guest will boot on next run."
    else
        echo "Reset aborted."
    fi
else
    echo "No active overlay found. Environment is already in its clean reference state."
fi