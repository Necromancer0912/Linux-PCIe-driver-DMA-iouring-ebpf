# Makefile — edu kernel module
#
# ── Build modes ──────────────────────────────────────────────────────────────
#
# 1. INSIDE THE GUEST (simplest — native compile):
#    Just run: make
#    The default KDIR points to the running kernel's build directory.
#
# 2. CROSS-COMPILE FROM MACOS (recommended for faster iteration):
#    First, locate your Buildroot output directory, then:
#
#    make KDIR=/path/to/buildroot/output/build/linux-6.x.y \
#         CROSS_COMPILE=/path/to/buildroot/output/host/bin/aarch64-buildroot-linux-gnu-
#
#    The exact kernel version and cross-compiler prefix depend on your
#    Buildroot configuration. After `make -j$(sysctl -n hw.ncpu)` completes:
#      - Kernel source: buildroot/output/build/linux-*/
#      - Cross-compiler: buildroot/output/host/bin/aarch64-buildroot-linux-gnu-gcc
#
# ── Usage inside the guest ───────────────────────────────────────────────────
#
#    insmod edu.ko
#    dmesg | tail -20        # should show probe + selftest results
#    ./edu_test              # run the userspace test binary
#    rmmod edu
#
# ─────────────────────────────────────────────────────────────────────────────

# Default: build against the currently running kernel (used inside the guest)
KDIR ?= /lib/modules/$(shell uname -r)/build

# Cross-compiler prefix — empty means use the host's native gcc
# Override this for cross-compilation:
#   CROSS_COMPILE=aarch64-buildroot-linux-gnu-
CROSS_COMPILE ?=

obj-m := edu.o

# Module build target
all:
	$(MAKE) -C $(KDIR) M=$(PWD) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Build the userspace test binary (native — run this inside the guest, or
# cross-compile by setting CC explicitly)
CC ?= gcc
edu_test: edu_test.c edu.h
	$(CC) -Wall -Wextra -O2 -o edu_test edu_test.c

.PHONY: all clean
