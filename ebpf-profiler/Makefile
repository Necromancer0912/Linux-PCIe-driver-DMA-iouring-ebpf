# Makefile for the EDU eBPF latency profiler
#
# Requires (inside the Ubuntu guest):
#   apt install clang llvm libbpf-dev linux-headers-$(uname -r) bpftool
#
# Step 1: Generate vmlinux.h (kernel type definitions for CO-RE)
#   sudo bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h
#
# Step 2: Build
#   make
#
# Step 3: Run
#   sudo insmod ../edu-driver/edu.ko
#   sudo ./edu_latency -i 3
#   # In another terminal: sudo ./edu_test

CLANG    := clang
LLC      := llc
BPFTOOL  := bpftool
CC       := gcc

ARCH     := $(shell uname -m | sed 's/x86_64/x86/' | sed 's/aarch64/arm64/')
LINUX_HDR:= /usr/src/linux-headers-$(shell uname -r)

BPF_CFLAGS := \
    -g -O2 -target bpf \
    -D__TARGET_ARCH_$(ARCH) \
    -I$(LINUX_HDR)/include \
    -I/usr/include/$(shell uname -m)-linux-gnu \
    -I.

USER_CFLAGS := -Wall -Wextra -O2 -g
USER_LIBS   := -lbpf -lelf -lz

.PHONY: all clean bpftrace_check

all: edu_latency

# ── Step 1: Compile BPF C → BPF object ──────────────────────────────────────
edu_latency.bpf.o: edu_latency.bpf.c vmlinux.h
	$(CLANG) $(BPF_CFLAGS) -c -o $@ $<
	@echo "[BPF] Compiled $<"

# ── Step 2: Generate BPF skeleton header ────────────────────────────────────
# The skeleton contains the BPF program embedded as a C array, plus
# typed accessors for all maps — no raw fd juggling needed.
edu_latency.skel.h: edu_latency.bpf.o
	$(BPFTOOL) gen skeleton $< > $@
	@echo "[BTF] Generated skeleton header $@"

# ── Step 3: Compile userspace loader ────────────────────────────────────────
edu_latency: edu_latency.c edu_latency.skel.h
	$(CC) $(USER_CFLAGS) -o $@ $< $(USER_LIBS)
	@echo "[CC]  Built $@"

# ── vmlinux.h — generate from running kernel ────────────────────────────────
vmlinux.h:
	@echo "Generating vmlinux.h from running kernel..."
	@if ! command -v bpftool >/dev/null 2>&1; then \
	    echo "ERROR: bpftool not found. Install with: apt install bpftool"; exit 1; fi
	sudo $(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@
	@echo "[BTF] Generated vmlinux.h ($(shell wc -l < vmlinux.h) lines)"

# ── Quick test: bpftrace version (no compilation needed) ─────────────────────
bpftrace_run: edu_latency.bt
	@echo "Launching bpftrace profiler (Ctrl-C to stop)..."
	sudo bpftrace edu_latency.bt

# ── Check bpftrace is installed ──────────────────────────────────────────────
bpftrace_check:
	@command -v bpftrace >/dev/null 2>&1 || \
	    (echo "bpftrace not found. Install: apt install bpftrace" && exit 1)
	@echo "bpftrace: OK  ($(shell bpftrace --version))"

clean:
	rm -f edu_latency edu_latency.bpf.o edu_latency.skel.h

# DO NOT delete vmlinux.h on clean — it takes a while to generate.
# Run 'make clean_all' to remove it.
clean_all: clean
	rm -f vmlinux.h
