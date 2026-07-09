# PCIe Kernel Driver + UART Framing Protocol
## Project Report

---

## 1. What the EDU Device Is

The `edu` device is a deliberately simple PCI peripheral built into QEMU for educational purposes. It exposes a 1 MB MMIO BAR (BAR0) at PCI IDs vendor `0x1234`, device `0x11e8`. Its register set covers exactly the features a driver author needs to learn: identity checking, a liveness round-trip, an asynchronous factorial computation, interrupt control, and a DMA engine with an onboard 4 KB buffer.

Because it ships inside QEMU itself (no ROM, no out-of-tree patch), any Linux kernel running under `qemu-system-*` with `-device edu` sees a real PCI device enumerated by the firmware and presented to the OS through the standard PCI bus layer. The driver code is therefore architecturally identical to a driver for real hardware — the only difference is the "hardware" runs in software.

The project runs on a Mac M4 Air using QEMU's HVF (Apple Hypervisor.framework) backend, which executes the aarch64 Linux guest at near-native speed. This is the correct, fast path for Apple Silicon; the TCG software-emulation path used by most x86 tutorials is unnecessary here.

---

## 2. Driver Architecture

### 2.1 Phase progression

The driver was built and tested incrementally, one capability layer at a time. Each phase was confirmed working before the next was added.

```
Phase 2: PCI probe skeleton
  └─ pci_enable_device → pci_request_region → pci_iomap
  └─ Verified: dmesg shows "EDU device probed, ID reg: 0x010000ed"

Phase 3: MMIO register self-test
  └─ ID register (bottom byte = 0xED ✓)
  └─ Liveness: write 0xdeadbeef, read ~0xdeadbeef ✓
  └─ Polled factorial: 5! = 120 ✓ (busy-wait, replaced in Phase 5)

Phase 4: Character device + ioctl
  └─ alloc_chrdev_region + cdev_init + cdev_add
  └─ device_create → udev auto-creates /dev/edu0
  └─ EDU_IOC_FACTORIAL, EDU_IOC_LIVENESS, EDU_IOC_DMA_TEST ioctls
  └─ edu_test.c userspace binary drives all operations

Phase 5: Interrupt-driven completion
  └─ request_irq with IRQF_SHARED
  └─ ISR reads interrupt status, acknowledges via 0x64
  └─ ioctl blocks on wait_queue instead of busy-polling
  └─ Verified: no CPU spike during factorial computation

Phase 6: DMA — bidirectional, verified
  └─ dma_set_mask(28-bit) — hardware constraint, not optional
  └─ dma_alloc_coherent → coherent kernel buffer
  └─ RAM→device: fill buffer with 0xAB, DMA into device's 0x40000
  └─ device→RAM: DMA device's 0x40000 into a second buffer
  └─ memcmp(buf1, buf2) == 0 → PASS
  └─ IRQ on completion (EDU_DMA_IRQ_ON_DONE) — same wait_queue as factorial
```

### 2.2 Key design decisions

**`IRQF_SHARED` in `request_irq`**: the QEMU virt machine puts multiple virtual devices on the same interrupt line. The ISR must check the device's own interrupt status register (`0x24`) and return `IRQ_NONE` if the interrupt belongs to someone else. Omitting this causes the kernel to incorrectly attribute foreign interrupts to the edu driver.

**`wait_queue` over polling**: the ioctl blocks with `wait_event_interruptible_timeout()` rather than spinning on the status register. This releases the CPU to other work while the device computes and keeps the ioctl responsive to signals. It is the standard pattern for interrupt-driven completion in real drivers.

**`dma_alloc_coherent` for the source buffer**: coherent allocation guarantees cache coherency between the CPU and device without manual cache maintenance calls. The read-back (device→RAM) buffer uses `dma_map_single` + `DMA_FROM_DEVICE` to demonstrate the streaming DMA path, which is more realistic for large transfers where a permanently-pinned coherent region would be wasteful.

**28-bit DMA mask**: the EDU device's DMA engine only generates 28-bit bus addresses. If `dma_set_mask(DMA_BIT_MASK(28))` is skipped, `dma_alloc_coherent` may return a buffer above the 256 MB boundary that the hardware cannot address, causing silent DMA failures. This was the first non-trivial debugging moment in the project.

---

## 3. Debugging Stories

### 3.1 The missing `pci_set_master()` call

**Symptom:** DMA transfers completed (the command register showed the operation finishing) but the destination buffer contained zeros — no data was moved.

**Root cause:** `pci_set_master()` was not called in `probe()`. Without bus-mastering enabled, the device cannot initiate DMA transactions onto the PCI bus even though it believes it has. The kernel does not enforce this at the API level — it is the driver's responsibility.

**Fix:** Added `pci_set_master(pdev)` immediately after the BAR mapping. DMA worked on the next load.

**Lesson:** DMA failures that look like "device ran but data is wrong" are almost always either a missing `pci_set_master`, a wrong DMA mask, or a cache coherency issue. Check them in that order.

### 3.2 Interrupt storm from missing acknowledgement

**Symptom:** After the first factorial computation, the kernel printed interrupt storm warnings and the guest became unresponsive. `dmesg` showed the ISR firing thousands of times per second.

**Root cause:** The ISR read the result correctly but did not write to the interrupt acknowledge register (`0x64`). The EDU device keeps the IRQ line asserted until the driver acknowledges. Since the line stayed high, the CPU kept re-entering the ISR.

**Fix:** Added `iowrite32(status, edu->mmio + EDU_REG_INTR_ACK)` at the end of the ISR, using the value read from the interrupt status register (`0x24`). The storm stopped immediately.

**Lesson:** Every interrupt controller has an acknowledge mechanism. Missing it is the most common ISR bug. On real hardware the consequences are identical — permanent IRQ assertion until acknowledged.

### 3.3 9p share permission errors

**Symptom:** Files written from macOS into `edu-driver/` appeared as unreadable inside the guest (`permission denied` on every file).

**Root cause:** `security_model=passthrough` maps host UIDs directly into the guest. The host macOS user (UID ~501) does not match any guest UID, so files appear owned by an unknown user with no permissions.

**Fix:** Changed `fsdev` to `security_model=mapped`. In this mode, 9p uses extended attributes to store ownership metadata independently of the host filesystem, and files appear with sensible guest permissions.

---

## 4. UART Protocol Design

### 4.1 Why a framing layer?

A raw UART byte stream has no structure — the receiver has no way to know where one message ends and the next begins, and there is no error detection. Real embedded communication links (PPP, HDLC, Modbus, CAN) all add a framing layer on top of the raw transport. This project implements one from scratch to demonstrate the design thinking.

### 4.2 Frame format

```
[0x7E][LEN][PAYLOAD...][XOR_CHECKSUM]
 1 byte  1 byte  LEN bytes     1 byte
```

- **0x7E** (`FRAME_START`): a byte that cannot appear unescaped anywhere else in the frame, making it an unambiguous delimiter.
- **LEN**: the number of payload bytes before byte-stuffing.
- **PAYLOAD**: application data.
- **XOR_CHECKSUM**: XOR of all payload bytes. Simple, deterministic, catches single-byte errors and many multi-byte patterns.

### 4.3 Byte stuffing

Any byte in LEN, PAYLOAD, or CHECKSUM that equals `0x7E` or `0x7D` is replaced by the two-byte sequence `[0x7D][byte XOR 0x20]`. The receiver reverses this inline. This is the same approach used in PPP (RFC 1662) and HDLC.

Without stuffing, a `0x7E` byte appearing inside a payload would cause the receiver to believe a new frame is starting mid-payload, corrupting all subsequent data silently.

### 4.4 Resync logic

The receiver discards bytes until it sees `0x7E`. On a checksum failure or a `0x7E` appearing in an unexpected position (framing error), it immediately returns to the scan-for-`0x7E` state. This means:

- **Power-on**: the receiver correctly handles joining a channel mid-stream.
- **Bit error in LEN or START**: at most one frame is lost; the receiver recovers automatically at the next `0x7E`.
- **Disconnection/reconnection**: no manual reset needed.

### 4.5 Testing strategy

| Test | What it proves |
|---|---|
| Basic loopback (5 frames) | The full encode/decode path works end-to-end |
| `0x7E`/`0x7D` in payload | Byte-stuffing is correctly applied and reversed |
| Injected wrong checksum | Corruption is detected; next valid frame still received |
| Garbage bytes before frame | Resync works on a mid-stream join |

The corruption test deliberately writes a raw frame with a flipped checksum bit directly to the file descriptor (bypassing `send_frame`), then sends a valid recovery frame. The receiver must reject the corrupt frame and then successfully decode the recovery frame — proving both detection and resync work together.

---

## 5. What Would Change for Real Hardware

| Topic | In this project | On real hardware |
|---|---|---|
| PCI enumeration | QEMU virtual bus; no physical slot | Same kernel API; firmware (UEFI/ACPI) presents device |
| IRQ | QEMU virtual interrupt controller | May use MSI/MSI-X instead of legacy INTx; `pci_alloc_irq_vectors()` replaces `request_irq(pdev->irq, ...)` |
| DMA | 28-bit mask is a device quirk | Real hardware often supports 64-bit; IOMMU remapping adds a translation layer |
| Cache coherency | Guaranteed by QEMU | Architecture-dependent; ARM requires explicit cache maintenance on non-coherent buses |
| UART | USB-TTL adapter on macOS via termios | Same termios API on embedded Linux; may need RS-485 direction control GPIO |
| Debugging | dmesg + QEMU monitor | JTAG, logic analyser, oscilloscope for physical signal verification |

---

## 6. Resume Bullet (Final)

> **PCIe/DMA Kernel Driver Development (Linux, QEMU/aarch64)**  
> Wrote a Linux kernel driver for a memory-mapped PCI device from scratch on a Mac M4 Air using QEMU HVF-accelerated aarch64 virtualization — BAR mapping, MMIO register access, interrupt-driven completion with `wait_queue`, and bidirectional DMA with coherent buffer allocation and `memcmp` verification — exposed via a custom char device and ioctl interface. Implemented a checksummed, byte-stuffed UART framing protocol in C using termios, including resync logic and corruption detection, tested via loopback.
