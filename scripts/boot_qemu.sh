#!/usr/bin/env bash
# boot_qemu.sh — Launch Ubuntu Server 24.04 arm64 guest with HVF acceleration.
#
# ── First-time setup (run once on macOS) ─────────────────────────────────────
#
#   brew install qemu cdrtools
#
#   # Download the Ubuntu 24.04 cloud image (~700 MB, takes a moment)
#   cd ~/pcie-driver-project    # or wherever you keep the project data
#   curl -LO https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-arm64.img
#
#   # Expand the disk (cloud images are shipped thin)
#   qemu-img resize noble-server-cloudimg-arm64.img +10G
#
#   # Build the cloud-init seed ISO (sets up login on first boot)
#   ./scripts/make_seed.sh
#
# ── After first boot ──────────────────────────────────────────────────────────
#
#   SSH in from a new terminal tab:
#     ssh -p 2222 ubuntu@localhost     # password: driver
#
#   Then run the in-guest setup:
#     bash /path/to/scripts/setup_guest.sh    # copy it in via scp first
#
# ── Usage ─────────────────────────────────────────────────────────────────────
#
#   ./scripts/boot_qemu.sh [path-to-image-dir]
#
#   Argument: directory containing noble-server-cloudimg-arm64.img and seed.iso
#   Defaults to ~/pcie-driver-project if not specified.

set -euo pipefail

IMAGE_DIR="${1:-$HOME/pcie-driver-project}"
DISK="$IMAGE_DIR/noble-server-cloudimg-arm64.img"
SEED="$IMAGE_DIR/seed.iso"
BIOS="/opt/homebrew/share/qemu/edk2-aarch64-code.fd"

# ── Pre-flight checks ─────────────────────────────────────────────────────────
if [[ ! -f "$DISK" ]]; then
    echo "ERROR: disk image not found: $DISK"
    echo "       Run the first-time setup steps at the top of this file."
    exit 1
fi

if [[ ! -f "$SEED" ]]; then
    echo "ERROR: seed ISO not found: $SEED"
    echo "       Run: ./scripts/make_seed.sh"
    exit 1
fi

if [[ ! -f "$BIOS" ]]; then
    echo "ERROR: UEFI firmware not found: $BIOS"
    echo "       Run: brew reinstall qemu"
    exit 1
fi

echo "Starting Ubuntu arm64 guest (HVF-accelerated)..."
echo "  disk:  $DISK"
echo "  seed:  $SEED"
echo "  bios:  $BIOS"
echo ""
echo "SSH access (once boot completes, ~30 seconds):"
echo "  ssh -p 2222 ubuntu@localhost    # password: driver"
echo ""
echo "Serial console is attached to this terminal."
echo "Press Ctrl-A X to exit QEMU."
echo "─────────────────────────────────────────────────────"

exec qemu-system-aarch64 \
    -M virt \
    -accel hvf \
    -cpu host \
    -m 4096 \
    -smp 4 \
    -bios "$BIOS" \
    -drive file="$DISK",if=virtio,format=qcow2 \
    -drive file="$SEED",if=virtio,format=raw,readonly=on \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0 \
    -device edu \
    -nographic
