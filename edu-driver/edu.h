/* SPDX-License-Identifier: GPL-2.0 */
/*
 * edu.h — EDU PCI device driver: shared definitions
 *
 * The EDU device is a deliberately simple PCI device built into QEMU for
 * educational purposes. It exposes a 1MB MMIO BAR (BAR0) with a small
 * set of registers for ID checking, liveness testing, factorial computation,
 * interrupt control, and DMA.
 *
 * PCI IDs: vendor 0x1234, device 0x11e8
 */

#ifndef EDU_H
#define EDU_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* ─── PCI identity ───────────────────────────────────────────────────────── */
#define EDU_VENDOR_ID   0x1234
#define EDU_DEVICE_ID   0x11e8

/* ─── BAR0 register offsets ──────────────────────────────────────────────── */

/*
 * 0x00  RO  Identification
 *           Format: 0xRRrr00ed  (bottom byte is always 0xed)
 *           RR = major version, rr = minor version
 */
#define EDU_REG_ID          0x00u

/*
 * 0x04  RW  Liveness check
 *           Write any 32-bit value; device returns bitwise NOT of it.
 *           Simplest "is this thing alive?" test.
 */
#define EDU_REG_LIVENESS    0x04u

/*
 * 0x08  RW  Factorial compute
 *           Write N (u32); device asynchronously computes N! and stores
 *           the result back at this offset.
 *           Poll/interrupt via STATUS register (0x20).
 */
#define EDU_REG_FACTORIAL   0x08u

/*
 * 0x20  RW  Status
 *           bit 0 = computation in progress (RO while set)
 *           bit 7 = raise interrupt when factorial finishes (RW)
 */
#define EDU_REG_STATUS      0x20u

/*
 * 0x24  RO  Interrupt status
 *           Tells which interrupt source is pending:
 *           bit 0 = factorial done
 *           bit 8 = DMA transfer done  (check QEMU hw/misc/edu.c for exact bits)
 */
#define EDU_REG_INTR_STATUS 0x24u

/*
 * 0x60  WO  Interrupt raise (software-triggerable test IRQ)
 * 0x64  WO  Interrupt acknowledge — MUST write this in ISR or IRQ line stays asserted
 */
#define EDU_REG_INTR_RAISE  0x60u
#define EDU_REG_INTR_ACK    0x64u

/*
 * DMA registers (64-bit, offsets 0x80+).
 * iowrite64/ioread64 used; on aarch64 these map to native MMIO.
 */
#define EDU_REG_DMA_SRC     0x80u   /* RW  DMA source address      */
#define EDU_REG_DMA_DST     0x88u   /* RW  DMA destination address */
#define EDU_REG_DMA_COUNT   0x90u   /* RW  Transfer byte count     */
#define EDU_REG_DMA_CMD     0x98u   /* RW  DMA command register    */

/*
 * Onboard DMA buffer inside the device — 4096 bytes at BAR0+0x40000.
 * Use this offset (not a RAM address) as src or dst for device-side DMA.
 */
#define EDU_DMA_DEV_BUF     0x40000u
#define EDU_DMA_BUF_SIZE    4096u

/* ─── Bit definitions ────────────────────────────────────────────────────── */

/* EDU_REG_STATUS bits */
#define EDU_STATUS_COMPUTING    BIT(0)  /* factorial in progress       */
#define EDU_STATUS_IRQ_ENABLE   BIT(7)  /* fire IRQ when factorial done */

/* EDU_REG_INTR_STATUS bits */
#define EDU_INTR_FACTORIAL_DONE BIT(0)
#define EDU_INTR_DMA_DONE       BIT(8)  /* verify against QEMU source  */

/* EDU_REG_DMA_CMD bits */
#define EDU_DMA_START           BIT(0)
#define EDU_DMA_DIR_TO_DEV      0u          /* bit1 = 0: RAM → device */
#define EDU_DMA_DIR_FROM_DEV    BIT(1)      /* bit1 = 1: device → RAM */
#define EDU_DMA_IRQ_ON_DONE     BIT(2)      /* fire IRQ when DMA completes */

/*
 * The EDU DMA engine only addresses 28-bit physical addresses.
 * dma_set_mask() must be called with DMA_BIT_MASK(28) — this is a real
 * hardware constraint, not a formality.
 */
#define EDU_DMA_MASK            DMA_BIT_MASK(28)

/* ─── ioctl interface ────────────────────────────────────────────────────── */

#define EDU_IOC_MAGIC   'E'

/*
 * EDU_IOC_FACTORIAL  _IOWR
 *   arg: pointer to u32
 *   Write: N to compute N!
 *   Read:  N! result (blocks until computation complete via IRQ)
 */
#define EDU_IOC_FACTORIAL   _IOWR(EDU_IOC_MAGIC, 0, __u32)
#define EDU_IOC_GET_DMA_SIZE _IOR(EDU_IOC_MAGIC, 3, __u32)

/* ─── io_uring passthrough commands (IORING_OP_URING_CMD) ────────────────
 *
 * Requires kernel ≥ 5.19 (Ubuntu 24.04 / kernel 6.8 — fine).
 *
 * Usage (userspace):
 *   struct io_uring ring;
 *   io_uring_queue_init(8, &ring, 0);
 *
 *   struct edu_uring_req req = { .op = EDU_URING_OP_DMA_TEST, .pattern = 0xAB };
 *   struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
 *   io_uring_prep_uring_cmd(sqe, fd, 0, 0);
 *   memcpy(sqe->cmd, &req, sizeof(req));
 *   io_uring_submit(&ring);
 *
 *   struct io_uring_cqe *cqe;
 *   io_uring_wait_cqe(&ring, &cqe);   // blocks until DMA done (no polling!)
 *   // cqe->res == 0 → success
 *
 * The key difference from ioctl:
 *   ioctl = synchronous, thread blocks in kernel until done.
 *   io_uring = asynchronous, thread is free to do other work; CQE posted
 *              when DMA completes via ISR → workqueue → io_uring_cmd_done.
 * ────────────────────────────────────────────────────────────────────── */

/* io_uring operation codes (put in edu_uring_req.op) */
#define EDU_URING_OP_DMA_TEST   1   /* Async bidirectional DMA round-trip */
#define EDU_URING_OP_FACTORIAL  2   /* Async factorial: cqe.res = N! */

/*
 * edu_uring_req — payload placed into the 80-byte SQE cmd[] field.
 * Userspace fills this and the driver reads it via io_uring_sqe_cmd().
 */
struct edu_uring_req {
    __u32 op;       /* EDU_URING_OP_* */
    __u32 arg;      /* DMA_TEST: fill pattern (byte); FACTORIAL: N */
    __u32 pad[2];   /* reserved, must be zero */
};

/*
 * EDU_IOC_LIVENESS   _IOWR
 *   arg: pointer to u32
 *   Write: any value
 *   Read:  bitwise NOT of that value (proves MMIO R/W path works)
 */
#define EDU_IOC_LIVENESS    _IOWR(EDU_IOC_MAGIC, 1, __u32)

/*
 * EDU_IOC_DMA_TEST   _IO
 *   No argument.
 *   Performs a full bidirectional DMA round-trip (RAM→device, device→RAM)
 *   and verifies data integrity with memcmp. Returns 0 on pass, -EIO on fail.
 */
#define EDU_IOC_DMA_TEST    _IO(EDU_IOC_MAGIC, 2)

#endif /* EDU_H */
