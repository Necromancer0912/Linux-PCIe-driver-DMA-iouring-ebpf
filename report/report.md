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

Phase 7: Asynchronous io_uring Integration
  └─ Added support for `IORING_OP_URING_CMD` via `.uring_cmd` file operation
  └─ Created deferred completion pipeline: ISR hard-IRQ -> workqueue -> `io_uring_cmd_done`
  └─ Implemented async DMA round-trip and async factorial commands
  └─ Verified: userspace thread performs CPU computation while DMA runs asynchronously

Phase 8: eBPF Interrupt and Latency Profiling
  └─ Instrumented kernel driver entry/exit hooks using kprobes and kretprobes
  └─ Measured ISR execution duration, DMA ioctl round-trip, and scheduler wake-up latency
  └─ Implemented both bpftrace script and full CO-RE libbpf binary with log2 histograms
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

## 6. Advanced Extensions

### 6.1 io_uring Asynchronous Passthrough

To bypass the synchronous blocking limitations of standard `ioctl` files, we implemented modern Linux `io_uring` passthrough support (`IORING_OP_URING_CMD`).

#### The Concurrency Challenge
The primary system design challenge when interfacing `io_uring` with a hardware device driver is **interrupt safety**. When the hardware completes a DMA or ALU calculation, it asserts the PCIe interrupt line, triggering the CPU's hard-IRQ handler (`edu_isr`).

However, standard kernel design dictates that the `io_uring` completion function `io_uring_cmd_done()` **cannot be executed from a hard-IRQ context** because it performs complex scheduling, context-saving, and locks page mappings which could cause deadlocks or trigger a kernel panic.

#### The Workqueue Pipeline
To solve this, we deferred the completion processing out of the hard-IRQ path and into a kernel-managed workqueue running in process context:

```
[ PCIe Device ] ──(Interrupt)──► [ edu_isr (Hard-IRQ) ]
                                         │
                                   (Schedule)
                                         ▼
[ Userspace CQE Woken ] ◄──(Process Context)── [ edu_uring_complete (Workqueue) ]
```

1. **Submission (`edu_uring_cmd`):** Userspace prepares a custom SQE payload containing the operation parameters and submits it to the ring. The driver receives this in `.uring_cmd`, programs the hardware registers, records the request pointer as pending, and immediately yields control back to the caller by returning `-EIOCBQUEUED`. The user thread is free to run other tasks.
2. **Detection (`edu_isr`):** When the hardware DMA engine finishes transferring data, it raises the MSI interrupt. The ISR catches this, identifies that an asynchronous `io_uring` command is pending, and schedules `uring_complete_work`.
3. **Execution (`edu_uring_complete`):** The workqueue handler runs in a separate thread context where blocking is safe. It grabs the pending command data under a mutex lock, executes the final verification step, and completes the lifecycle by calling `io_uring_cmd_done()`. Userspace is then notified via a CQE.

---

### 6.2 eBPF Interrupt and Scheduler Profiling

We instrumented the driver to measure latency at three key points under load, implementing both a lightweight `bpftrace` diagnostic script and a structured libbpf-based C binary using Compile-Once, Run-Everywhere (CO-RE) techniques.

```
       [ Userspace ioctl/uring Submission ]
                        │
                        ├─► Probe: edu_ioctl entry (Record T1)
                        ▼
                 [ Driver ISR ]
                        │
                        ├─► Probe: edu_isr entry (Record T2)
                        ├─► Probe: edu_isr exit  (Measure ISR Duration: T_isr = exit - T2)
                        ▼
              [ wake_up() executed ]
                        │
                        ├─► Probe: __wake_up_common (Record T3)
                        ▼
            [ Thread wakes up & resumes ]
                        │
                        ├─► Probe: finish_wait (Measure wake-up latency: T_sched = resume - T3)
                        ▼
       [ Userspace ioctl/uring Completion ]
                        │
                        ├─► Probe: edu_ioctl exit (Measure Round-trip: T_rt = exit - T1)
```

#### Monitored Probes
- **kprobe / kretprobe on `edu_isr`:** Records the total execution time of the hard-IRQ handler. A lean handler must complete in less than 5 microseconds to ensure system responsiveness and prevent CPU starvation.
- **kprobe / kretprobe on `edu_ioctl`:** Traces the total elapsed round-trip time of a DMA transfer from user space initiation to driver return.
- **kprobe on `__wake_up_common` and `finish_wait`:** Tracks scheduler latency. By comparing when the ISR calls the wake-up notifier to when the waiting thread actually executes its next instruction, we measure the system's task scheduling overhead under load.

#### Data Aggregation
The BPF programs aggregate timestamps locally in the kernel using high-performance hash tables (`BPF_MAP_TYPE_HASH`) and log2 arrays (`BPF_MAP_TYPE_ARRAY`). The userspace manager polls these maps, calculates cumulative metrics, and renders detailed ASCII bar charts showing latency distributions and P50/P95/P99 percentiles.
