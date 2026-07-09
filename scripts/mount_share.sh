#!/usr/bin/env bash
# setup_guest.sh — Run INSIDE the Ubuntu guest after first boot.
#
# Copy this into the VM first:
#   scp -P 2222 scripts/setup_guest.sh ubuntu@localhost:~
#
# Then run it:
#   ssh -p 2222 ubuntu@localhost
#   bash ~/setup_guest.sh
#
# What it does:
#   1. Installs the kernel driver toolchain (gcc, make, kernel headers)
#   2. Installs debugging tools (gdb, lspci, minicom, socat)
#   3. Verifies the EDU device is visible on the PCI bus
#   4. Prints next steps

set -euo pipefail

echo "═══════════════════════════════════════════════"
echo "  PCIe driver guest setup — Ubuntu 24.04 arm64"
echo "═══════════════════════════════════════════════"

# ── 1. Update and install toolchain ─────────────────────────────────────────
echo ""
echo "[1/3] Installing build toolchain and debug tools..."
sudo apt-get update -qq
sudo apt-get install -y \
    build-essential \
    linux-headers-$(uname -r) \
    git \
    gdb \
    pciutils \
    minicom \
    socat \
    gcc \
    make

echo "      Kernel headers: /lib/modules/$(uname -r)/build"
echo "      Cross-compile not needed — we build natively inside the guest."

# ── 2. Verify EDU device ─────────────────────────────────────────────────────
echo ""
echo "[2/3] Verifying EDU PCI device..."
if lspci | grep -q "1234:11e8"; then
    echo "      ✓ EDU device found: $(lspci | grep '1234:11e8')"
else
    echo "      ✗ EDU device NOT found."
    echo "        Make sure QEMU was started with -device edu"
    echo "        Check: lspci -v"
fi

# ── 3. Create a workspace directory ──────────────────────────────────────────
echo ""
echo "[3/3] Setting up workspace..."
mkdir -p ~/driver-workspace
cat <<'EOF' > ~/driver-workspace/README
Copy your driver source files into this directory.

From macOS (replace 2222 if you changed the port):
  scp -P 2222 -r /Users/you/pcie-driver-project/new_proj/edu-driver ubuntu@localhost:~/driver-workspace/
  scp -P 2222 -r /Users/you/pcie-driver-project/new_proj/uart-protocol ubuntu@localhost:~/driver-workspace/

Build the kernel module:
  cd ~/driver-workspace/edu-driver
  make
  sudo insmod edu.ko
  dmesg | tail -20

Build the UART test (inside the guest, uses gcc):
  cd ~/driver-workspace/uart-protocol
  make
EOF

echo ""
echo "═══════════════════════════════════════════════"
echo "  Setup complete. Next steps:"
echo ""
echo "  From macOS, copy your source into the guest:"
echo "    scp -P 2222 -r ./edu-driver   ubuntu@localhost:~/driver-workspace/"
echo "    scp -P 2222 -r ./uart-protocol ubuntu@localhost:~/driver-workspace/"
echo ""
echo "  Inside the guest, build and load the driver:"
echo "    cd ~/driver-workspace/edu-driver"
echo "    make"
echo "    sudo insmod edu.ko"
echo "    dmesg | tail -20"
echo ""
echo "  Confirm MSI interrupt registered:"
echo "    cat /proc/interrupts | grep edu"
echo "    lspci -vvv | grep -A5 'Capabilities: \[.*\] MSI'"
echo "═══════════════════════════════════════════════"
