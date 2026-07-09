# Makefile — UART framing protocol
#
# Builds natively inside the Linux guest (gcc) OR on macOS (clang).
# Auto-detects the platform.
#
# ── Build (inside the guest) ──────────────────────────────────────────────────
#
#   cd ~/driver-workspace/uart-protocol
#   make
#
# ── Run (inside the guest) ───────────────────────────────────────────────────
#
#   # Option A: socat pseudoterminal loopback (recommended, no hardware needed)
#   #   Terminal 1:
#   socat -d -d pty,raw,echo=0 pty,raw,echo=0
#   #   Note the two /dev/pts/N paths it prints (e.g. /dev/pts/2 and /dev/pts/3)
#   #   Terminal 2:
#   sudo ./uart_test /dev/pts/2 /dev/pts/3
#
#   # Option B: QEMU virtual serial loopback (TX wired to RX in QEMU)
#   #   Device path inside guest: /dev/ttyAMA1 (second PL011 UART)
#   #   Requires QEMU launched with an extra -serial flag (see README)
#   sudo ./uart_test /dev/ttyAMA1
#
# ── Build on macOS (clang) ────────────────────────────────────────────────────
#
#   The same Makefile works on macOS:
#   make   (uses clang automatically on macOS)

UNAME := $(shell uname)
ifeq ($(UNAME), Darwin)
    CC = clang
else
    CC = gcc
endif

CFLAGS  = -Wall -Wextra -Wshadow -O2 -std=c11
SRCS    = uart_frame.c uart_test.c
TARGET  = uart_test

$(TARGET): $(SRCS) uart_frame.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET) *.o

.PHONY: clean
