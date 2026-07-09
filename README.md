# PCIe Kernel Driver & UART Framing Protocol

<div align="center">

**A low-level Linux systems project built from scratch on Apple Silicon**

*Kernel-space PCIe driver · MSI interrupt handling · Coherent DMA · Custom serial framing protocol*

</div>

---

## What Is This?

This project is a **Linux kernel-mode device driver** written entirely from scratch, targeting QEMU's virtual `edu` PCI device — a purpose-built teaching peripheral that exposes real PCI bus mechanics (BAR mapping, MMIO registers, interrupts, DMA) in a safe, reproducible environment.

Alongside it, we implement a **custom link-layer framing protocol** over UART — the same class of protocol used in embedded communication stacks (PPP, HDLC, Modbus) — to demonstrate serial communication design from first principles.

No pre-written driver frameworks. No libraries. Every line of kernel code is handwritten.

---

## Why? — Motivation

Modern software engineering increasingly abstracts away the hardware. Most developers never write a line of code that runs in kernel space, never touch a DMA buffer, and never implement their own interrupt handler. But the engineers who *can* do this — who understand the full stack from silicon to syscall — are the ones who can debug what nobody else can, design what nobody else can, and go where others can't.

**Specific goals:**
- Understand the complete lifecycle of a PCI device driver: probe → resource allocation → interrupt registration → DMA → teardown
- Learn the Linux kernel's DMA API and why cache coherency matters
- Understand the tradeoffs between polling, interrupt-driven I/O, and DMA (latency vs CPU utilization)
- Design a serial framing protocol from scratch — byte stuffing, checksums, resync — and understand why each piece exists
- Build resume-worthy, interviewer-defensible systems work: code you can explain line by line under questioning

---

## How We Built It — Architecture

```
╔══════════════════════════════════════════════════════════════════════════╗
║  macOS Host  ·  Apple M4 Air  ·  Apple Hypervisor.framework (HVF)      ║
║                                                                          ║
║  ┌──────────────────────────────────────────────────────────────────┐  ║
║  │  QEMU  (HVF-accelerated · arm64 · near-native speed)             │  ║
║  │                                                                   │  ║
║  │  ┌─────────────────────────────────────────────────────────┐    │  ║
║  │  │  Ubuntu 24.04 arm64 Guest                                │    │  ║
║  │  │                                                          │    │  ║
║  │  │   ┌──────────────────┐      ┌────────────────────────┐  │    │  ║
║  │  │   │   Kernel Space   │      │  QEMU Virtual PCI Bus  │  │    │  ║
║  │  │   │                  │      │                        │  │    │  ║
║  │  │   │  ┌────────────┐  │ MMIO │  ┌──────────────────┐  │  │    │  ║
║  │  │   │  │  edu.ko    │◄─┼──────┼─►│   edu device     │  │  │    │  ║
║  │  │   │  │            │  │      │  │  ┌────────────┐  │  │  │    │  ║
║  │  │   │  │ PCI probe  │  │ MSI  │  │  │ MMIO regs  │  │  │  │    │  ║
║  │  │   │  │ BAR map    │◄─┼──────┼──│  │ Factorial  │  │  │  │    │  ║
║  │  │   │  │ MSI vector │  │      │  │  │ ALU        │  │  │  │    │  ║
║  │  │   │  │ DMA engine │◄─┼──────┼─►│  │ DMA engine │  │  │  │    │  ║
║  │  │   │  │ wait_queue │  │      │  │  │ 4KB buffer │  │  │  │    │  ║
║  │  │   │  │ /dev/edu0  │  │      │  └──────────────────┘  │  │    │  ║
║  │  │   │  └─────┬──────┘  │      └────────────────────────┘  │    │  ║
║  │  │   │        │ ioctl   │                                   │    │  ║
║  │  │   └────────┼─────────┘                                   │    │  ║
║  │  │            │                                             │    │  ║
║  │  │   ┌────────▼────────┐    ┌──────────────────────────┐   │    │  ║
║  │  │   │  Userspace      │    │  uart_test               │   │    │  ║
║  │  │   │  edu_test       │    │  socat pty pair /        │   │    │  ║
║  │  │   │  (liveness,     │    │  /dev/ttyAMA1 loopback   │   │    │  ║
║  │  │   │   factorial,    │    │  byte-stuffed frames     │   │    │  ║
║  │  │   │   DMA verify)   │    │  XOR checksum + resync   │   │    │  ║
║  │  │   └─────────────────┘    └──────────────────────────┘   │    │  ║
║  │  └─────────────────────────────────────────────────────────┘    │  ║
║  └──────────────────────────────────────────────────────────────────┘  ║
╚══════════════════════════════════════════════════════════════════════════╝
```

**Why Apple Silicon + QEMU + HVF?**

Running arm64 Linux inside QEMU with HVF (Apple Hypervisor.framework) gives near-native execution speed on M4 — the kernel compiles fast, module load/unload cycles are instant, and the entire environment is self-contained on one machine with no external hardware. The driver code itself is architecture-independent C; the only thing that changes between this and a real PCIe card is the silicon under the BAR.

---

## Tech Stack

| Layer | Technology | Purpose |
|---|---|---|
| Host machine | macOS 15, Apple M4 Air | Development environment |
| Hypervisor | QEMU 9.x + Apple HVF | arm64 Linux VM at near-native speed |
| Guest OS | Ubuntu Server 24.04 arm64 | Real kernel, real apt, matching headers |
| Virtual PCI device | QEMU `-device edu` | Purpose-built hardware-accurate PCI device for driver dev |
| Kernel APIs | `linux/pci.h`, `linux/interrupt.h`, `linux/dma-mapping.h`, `linux/cdev.h` | The actual driver subsystem interfaces |
| Interrupt model | MSI (Message Signalled Interrupts) via `pci_alloc_irq_vectors` | Modern, non-shared, per-device interrupt delivery |
| DMA model | Coherent DMA via `dma_alloc_coherent` + streaming via `dma_map_single` | Bidirectional data transfer with hardware address constraints |
| Userspace interface | Custom char device + ioctl (`/dev/edu0`) | Clean user↔kernel boundary |
| Serial protocol | POSIX `termios`, raw mode, 115200 8N1 | Hardware-accurate serial configuration |
| Protocol framing | Custom byte-stuffed, XOR-checksummed frame format | Link-layer reliability on top of raw serial |
| Debugging | `dmesg`, `lspci -vvv`, `/proc/interrupts`, `/proc/iomem`, QEMU gdbserver | Full kernel-level observability |
| Version control | Git with logical per-phase commits | Auditable, incremental build history |

---

## What We Built — Features

### Kernel Driver (`edu-driver/`)

| Feature | Implementation | Verified by |
|---|---|---|
| PCI device enumeration | `pci_probe` with `MODULE_DEVICE_TABLE` | `dmesg` shows device found on `insmod` |
| BAR0 MMIO mapping | `pci_request_region` + `pci_iomap` | `/proc/iomem` shows region reserved |
| Register self-test | ID check (0xED), liveness (bitwise NOT), polled factorial | `dmesg` selftest output |
| MSI interrupt vector | `pci_alloc_irq_vectors(PCI_IRQ_MSI)` + `request_irq` | `cat /proc/interrupts \| grep edu` |
| Interrupt-driven completion | `wait_queue_head_t` — ioctl blocks, ISR wakes | No CPU spike during factorial computation |
| Bidirectional DMA | RAM→device then device→RAM with IRQ-on-completion | `memcmp` of source and destination buffers |
| 28-bit DMA address constraint | `dma_set_mask(DMA_BIT_MASK(28))` | Required by `edu` hardware spec |
| Char device interface | `/dev/edu0` via `cdev` + `device_create` | `./edu_test` userspace binary |
| ioctl API | `EDU_IOC_FACTORIAL`, `EDU_IOC_LIVENESS`, `EDU_IOC_DMA_TEST` | All tests pass in `edu_test` |
| Clean resource lifecycle | Every `alloc` paired with matching `free` in unwind and remove | Repeated `insmod`/`rmmod` cycles, no `dmesg` warnings |

### UART Framing Protocol (`uart-protocol/`)

| Feature | Detail |
|---|---|
| Frame format | `[0x7E][LEN][PAYLOAD...][XOR_CHECKSUM]` |
| Byte stuffing | PPP/HDLC-style: `0x7E` and `0x7D` in payload escaped as `[0x7D][byte^0x20]` |
| Resync logic | Receiver scans for `0x7E` on corruption or mid-stream start — recovers in ≤1 frame |
| Corruption detection | XOR checksum over payload — rejects frame, resyncs, continues |
| Test coverage | Basic loopback · `0x7E`/`0x7D` in payload · corrupted checksum + resync · garbage injection |
| Transport-agnostic | Works on `/dev/pts/N` (socat), `/dev/ttyAMA1` (QEMU virtual), or real USB-TTL hardware |

---

## What We Achieved — Outcomes

```
✓  Written a Linux PCI kernel driver entirely from scratch
✓  Mapped and accessed PCI BAR0 MMIO registers from kernel space
✓  Registered an MSI interrupt vector and written an ISR
✓  Performed coherent DMA transfers in both directions, verified with memcmp
✓  Built a userspace ↔ kernel interface via char device and ioctl
✓  Designed a serial framing protocol with byte stuffing and error recovery
✓  Deployed everything in a reproducible, HVF-accelerated arm64 Linux VM on macOS
✓  Documented debugging stories — the bugs found and how they were fixed
```

**Bugs found and understood (these are what interviewers actually ask about):**

- Missing `pci_set_master()` → DMA registers accepted writes but zero data moved *(bus-mastering must be explicitly enabled)*
- Missing ISR acknowledge write to `0x64` → interrupt storm, guest locked up *(every IRQ controller requires explicit EOI)*
- `IRQF_SHARED` replaced with `pci_alloc_irq_vectors(PCI_IRQ_MSI)` → cleaner, per-device vector, matches how `edu` device actually fires interrupts internally
- 9p file share `security_model=passthrough` → all files unreadable in guest *(host UID 501 doesn't map to guest — use `mapped` mode)*

---

## Practical Applications

This project directly mirrors production engineering work in:

| Industry | Real-world parallel |
|---|---|
| **Cloud infrastructure (AWS, Google, Azure)** | NIC and NVMe drivers are PCIe devices; every Nitro card, every RDMA adapter uses the same probe→MMIO→MSI→DMA pattern |
| **Autonomous vehicles (Waymo, Cruise)** | LiDAR and radar sensors connect via PCIe; their drivers use identical DMA ring-buffer patterns |
| **High-frequency trading** | Custom PCIe FPGA cards use kernel bypass DMA (DPDK) — understanding coherent DMA is a prerequisite |
| **Embedded / IoT firmware** | UART framing protocols of exactly this design appear in every GNSS module, Bluetooth HCI stack (H4 protocol), and Modbus RTU device |
| **GPU drivers (NVIDIA, AMD)** | Open-source kernel drivers (amdgpu, nouveau) are PCI drivers with DMA — same APIs, vastly larger scale |
| **Storage (NVMe)** | The Linux NVMe driver in `drivers/nvme/host/pci.c` uses `pci_alloc_irq_vectors`, coherent DMA, and wait_queues — exactly what we built, at production scale |

---

## Advanced Extensions

### ✅ Already complete
- MSI interrupts (vs. legacy INTx)
- Bidirectional DMA with IRQ-driven completion
- Custom ioctl interface with userspace test binary
- UART byte stuffing + corruption detection + resync
- **`mmap` zero-copy DMA buffer** — `dma_mmap_coherent()` exposes the coherent buffer directly into userspace; write to the pointer, trigger `EDU_IOC_DMA_TEST`, verify — zero `copy_to_user` in the path
- **Reliable UART transport** (`uart-protocol/reliable/`) — stop-and-wait protocol with DATA/ACK/NACK frame types, 1-byte sequence numbers, retransmit on timeout, duplicate detection, session statistics

### 🚀 Remaining advanced directions

**1. `debugfs` register dump**
Expose all device register values at `/sys/kernel/debug/edu/regs` — live introspection without touching driver code. Standard tool for real driver debugging.

**2. DMA performance benchmark**
Measure sustained DMA throughput (MB/s) and interrupt-to-wake latency (µs) as buffer size varies. Produces concrete numbers to quote — far stronger than "I implemented DMA."

**3. KUnit in-kernel unit tests**
Write a kernel test module using the KUnit framework that tests the driver's DMA logic and interrupt acknowledgement paths without needing a userspace binary — the same approach used in upstream kernel development.

**4. Sliding-window UART**
Extend the reliable transport from stop-and-wait (window=1) to a multi-frame window (HDLC extended mode, same as TCP). Extend `tx_seq`/`rx_seq` to track outstanding unACK'd frames — the architecture is already designed for this.

---

## Repository Structure

```
new_proj/
├── edu-driver/
│   ├── edu.h           ← register map, bit defs, DMA mask, ioctl commands
│   ├── edu.c           ← full kernel module (probe, MMIO, MSI, DMA, mmap)
│   ├── edu_test.c      ← userspace tests: liveness, factorial×5, DMA, mmap
│   └── Makefile        ← native in-guest build
├── uart-protocol/
│   ├── uart_frame.h    ← base frame format spec + API
│   ├── uart_frame.c    ← send_frame, recv_frame, byte-stuffing, resync
│   ├── uart_test.c     ← 4-test harness (loopback, stuffing, corruption, resync)
│   ├── Makefile        ← auto-detects Linux (gcc) vs macOS (clang)
│   └── reliable/
│       ├── uart_reliable.h      ← reliable transport API + frame format
│       ├── uart_reliable.c      ← DATA/ACK/NACK, seq numbers, retransmit
│       ├── uart_reliable_test.c ← round-trip, multi-frame, stats
│       └── Makefile
├── scripts/
│   ├── boot_qemu.sh    ← boots Ubuntu 24.04 arm64 VM with HVF + EDU device
│   ├── make_seed.sh    ← builds cloud-init seed.iso (run once on macOS)
│   ├── mount_share.sh  ← in-guest setup: apt install, lspci verify, workspace
│   └── cloud-init/
│       ├── user-data   ← ubuntu/driver login, SSH auth, disk auto-grow
│       └── meta-data   ← cloud-init instance metadata
├── buildroot-config/
│   └── edu_defconfig   ← alternative Buildroot config (if Ubuntu path unused)
└── report/
    └── report.md       ← technical writeup: architecture, debugging stories, design rationale
```

---

## Getting Started

### Prerequisites (macOS)

```bash
brew install qemu cdrtools
xcode-select --install      # gives you clang + git
```

### Step 1 — First-time image setup

```bash
mkdir ~/pcie-driver-project && cd ~/pcie-driver-project
curl -LO https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-arm64.img
qemu-img resize noble-server-cloudimg-arm64.img +10G
./scripts/make_seed.sh      # builds seed.iso from cloud-init config
```

### Step 2 — Boot the VM

```bash
./scripts/boot_qemu.sh      # serial console appears in this terminal
```

### Step 3 — SSH in and set up the guest

```bash
# New terminal tab:
ssh -p 2222 ubuntu@localhost          # password: driver

# Copy setup script and run it:
scp -P 2222 scripts/mount_share.sh ubuntu@localhost:~
bash ~/mount_share.sh                 # installs gcc, kernel headers, pciutils, socat
```

### Step 4 — Copy source and build the driver

```bash
scp -P 2222 -r edu-driver    ubuntu@localhost:~/driver-workspace/
scp -P 2222 -r uart-protocol ubuntu@localhost:~/driver-workspace/

# Inside guest:
cd ~/driver-workspace/edu-driver
make
sudo insmod edu.ko
dmesg | tail -20
```

Expected `dmesg` output:
```
edu 0000:00:02.0: selftest: ID = 0x010000ed [OK]
edu 0000:00:02.0: selftest: liveness 0xdeadbeef → 0x21524110 [OK]
edu 0000:00:02.0: selftest: 5! = 120 [OK]
edu 0000:00:02.0: EDU driver loaded: BAR0@..., MSI IRQ=..., DMA buf phys=0x...
```

### Step 5 — Run userspace tests

```bash
gcc -Wall -O2 -o edu_test edu_test.c
sudo ./edu_test
```

Expected output:
```
[PASS] liveness: wrote 0xdeadbeef, got 0x21524110
[PASS] factorial: 1! = 1
[PASS] factorial: 5! = 120
[PASS] factorial: 7! = 5040
[PASS] factorial: 10! = 3628800
[PASS] factorial: 12! = 479001600
[PASS] DMA round-trip (RAM→device→RAM memcmp verified)
[PASS] mmap: 4096-byte DMA buffer mapped at 0x..., 0xAB pattern survives zero-copy round-trip
All tests passed.
```

### Step 6 — Run UART tests (inside guest)

```bash
cd ~/driver-workspace/uart-protocol
make

# Terminal A inside guest:
socat -d -d pty,raw,echo=0 pty,raw,echo=0
# Note the two /dev/pts/N paths printed

# Terminal B inside guest:
sudo ./uart_test /dev/pts/2 /dev/pts/3
```

---

## EDU Device Register Map

| Offset | R/W | Register | Behaviour |
|--------|-----|----------|-----------|
| `0x00` | RO | ID | Bottom byte always `0xED`; upper bytes = version |
| `0x04` | RW | Liveness | Write any value; device returns bitwise NOT |
| `0x08` | RW | Factorial | Write N; device computes N! asynchronously |
| `0x20` | RW | Status | bit0 = computing; bit7 = enable IRQ on done |
| `0x24` | RO | IRQ Status | bit0 = factorial done; bit8 = DMA done |
| `0x60` | WO | IRQ Raise | Software-trigger test interrupt |
| `0x64` | WO | IRQ Ack | **Write here in ISR — clears IRQ line** |
| `0x80` | RW | DMA Src | 64-bit source bus address |
| `0x88` | RW | DMA Dst | 64-bit destination bus address |
| `0x90` | RW | DMA Count | Transfer byte count |
| `0x98` | RW | DMA Cmd | bit0=start, bit1=direction, bit2=IRQ on done |
| `0x40000` | — | DMA Buffer | 4 KB device-internal buffer |

---

## Debugging Reference

```bash
# Confirm EDU device is on the PCI bus
lspci -vvv | grep -A 10 "1234:11e8"

# Check MSI interrupt is registered
cat /proc/interrupts | grep edu

# Confirm BAR0 is reserved while module is loaded
cat /proc/iomem | grep edu

# Live kernel log
dmesg -w

# Load/unload cycle (clean unload means no resource leaks)
sudo insmod edu.ko && sudo rmmod edu && dmesg | tail -5

# QEMU gdbserver (kernel-level breakpoints)
# Boot QEMU with -s -S, then from macOS:
# gdb vmlinux → target remote :1234
```

---

## Resume Bullet

> **PCIe Device Driver Development — Linux Kernel, QEMU/arm64**
> Wrote a Linux kernel driver from scratch for a memory-mapped PCI device: BAR mapping, MMIO register access, MSI interrupt handling with `wait_queue`-based completion, and bidirectional coherent DMA verified with `memcmp`. Exposed the device via a char device and ioctl interface. Implemented a checksummed, byte-stuffed UART framing protocol in C using `termios`, with resync and corruption detection. Deployed in an HVF-accelerated arm64 QEMU environment on Apple Silicon.

---

## Authors

Built by:

- **Sayan** — [GitHub](https://github.com/Necromancer0912)
- **Senjuti Ghosal** — [GitHub](https://github.com/senjuti09)

---

## References

| Resource | Purpose |
|---|---|
| [QEMU `hw/misc/edu.c`](https://github.com/qemu/qemu/blob/master/hw/misc/edu.c) | Ground truth for the device register map — read this first |
| [QEMU EDU device spec](https://www.qemu.org/docs/master/specs/edu.html) | Official documentation |
| *Linux Device Drivers, 3rd Ed.* (Corbet, Rubini, Kroah-Hartman) | PCI and DMA chapters — free online |
| `Documentation/PCI/pci.rst` | Linux kernel source |
| `Documentation/core-api/dma-api.rst` | Linux kernel source |
| `man 3 termios` | UART configuration reference |
