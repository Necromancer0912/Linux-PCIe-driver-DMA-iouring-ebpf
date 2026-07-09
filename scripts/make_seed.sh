#!/usr/bin/env bash
# make_seed.sh — Build the cloud-init seed ISO for Ubuntu first-boot config.
#
# Run this ONCE on macOS before the first `boot_qemu.sh` launch.
# Requires: brew install cdrtools    (provides mkisofs)
#
# Usage:
#   ./scripts/make_seed.sh [output-dir]
#   output-dir defaults to ~/pcie-driver-project

set -euo pipefail

OUTPUT_DIR="${1:-$HOME/pcie-driver-project}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CI_DIR="$SCRIPT_DIR/cloud-init"

mkdir -p "$OUTPUT_DIR"

if ! command -v mkisofs &>/dev/null; then
    echo "ERROR: mkisofs not found. Install with: brew install cdrtools"
    exit 1
fi

echo "Building cloud-init seed ISO..."
echo "  user-data: $CI_DIR/user-data"
echo "  meta-data: $CI_DIR/meta-data"
echo "  output:    $OUTPUT_DIR/seed.iso"

mkisofs \
    -output "$OUTPUT_DIR/seed.iso" \
    -volid cidata \
    -joliet \
    -rock \
    "$CI_DIR/user-data" \
    "$CI_DIR/meta-data"

echo "Done: $OUTPUT_DIR/seed.iso"
