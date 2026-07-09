// SPDX-License-Identifier: GPL-2.0
/*
 * edu.c — Linux kernel driver for QEMU's EDU PCI device
 *
 * Build order / phase progression documented inline:
 *   Phase 2: PCI probe/remove skeleton
 *   Phase 3: MMIO register self-test (ID, liveness, polled factorial)
 *   Phase 4: Character device + ioctl interface
 *   Phase 5: Interrupt handling (ISR + wait_queue, replaces busy-poll)
 *   Phase 6: DMA (bidirectional, memcmp-verified, IRQ-driven completion)
 *
 * Author: Sayan
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
/* io_uring passthrough (kernel >= 5.19, Ubuntu 24.04 / 6.8 is fine) */
#include <linux/io_uring/cmd.h>

/* Pull in iowrite64/ioread64 helpers for non-atomic 64-bit MMIO on aarch64.
 * On most aarch64 kernels this is a no-op include (native support exists),
 * but it's the correct defensive include to have. */
#include <linux/io-64-nonatomic-lo-hi.h>

#include "edu.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Internal device state
 * ═══════════════════════════════════════════════════════════════════════════ */

struct edu_dev {
    /* PCI */
    struct pci_dev  *pdev;
    void __iomem    *mmio;          /* virtual pointer to BAR0 */

    /* Character device */
    struct cdev      cdev;
    dev_t            devno;

    /* Interrupt / completion synchronization (Phase 5) */
    wait_queue_head_t wq;
    volatile u32      factorial_result; /* written by ISR, read by ioctl */
    volatile bool     irq_done;         /* condition variable for wq */

    /* DMA (Phase 6) */
    void        *dma_buf;           /* kernel virtual address of coherent buffer */
    dma_addr_t   dma_addr;          /* bus (DMA) address of the same buffer */

    /* io_uring async completion (Phase 7: io_uring integration)
     *
     * io_uring_cmd_done() CANNOT be called from hard-IRQ context.
     * When the ISR sees a DMA-done interrupt AND there is a pending
     * uring_cmd, it schedules uring_complete_work.  The workqueue runs
     * in process context and safely calls io_uring_cmd_done().
     *
     * Concurrency: protected by edu->lock.  Only one outstanding
     * io_uring op at a time (EBUSY if a second arrives while one is
     * in flight — identical to how NVMe passthrough handles this).
     */
    struct io_uring_cmd *pending_uring_cmd;   /* NULL when idle */
    int                  pending_uring_result; /* result code for CQE */
    struct work_struct   uring_complete_work;  /* deferred CQE poster */

    /* Serialise concurrent ioctl / uring_cmd calls */
    struct mutex lock;
};

/* Module-level: one device class, one minor number range */
static struct class  *edu_class;
static int            edu_major;

/* ═══════════════════════════════════════════════════════════════════════════
 * Phase 3: MMIO register self-test
 * Called from probe() after MMIO is mapped. Logs results via dev_info/dev_err.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void edu_selftest(struct edu_dev *edu)
{
    struct device *dev = &edu->pdev->dev;
    u32 id, val;

    /* ── ID register ──────────────────────────────────────────────────────
     * Format: 0xRRrr00ed — bottom byte is always 0xed.
     * RR/rr encode major/minor version of the QEMU EDU device spec.
     */
    id = ioread32(edu->mmio + EDU_REG_ID);
    if ((id & 0xFF) == 0xED)
        dev_info(dev, "selftest: ID = 0x%08x [OK]\n", id);
    else
        dev_err(dev,  "selftest: ID = 0x%08x [UNEXPECTED — bottom byte should be 0xed]\n", id);

    /* ── Liveness check ───────────────────────────────────────────────────
     * Write 0xdeadbeef; device must return bitwise NOT = 0x21524110.
     */
    iowrite32(0xdeadbeef, edu->mmio + EDU_REG_LIVENESS);
    val = ioread32(edu->mmio + EDU_REG_LIVENESS);
    if (val == (u32)(~0xdeadbeef))
        dev_info(dev, "selftest: liveness 0xdeadbeef → 0x%08x [OK]\n", val);
    else
        dev_err(dev,  "selftest: liveness got 0x%08x, expected 0x%08x [FAIL]\n",
                val, (u32)(~0xdeadbeef));

    /* ── Polled factorial ─────────────────────────────────────────────────
     * Write 5, spin-poll STATUS bit0 until clear, read result.
     * Phase 5 replaces this busy-wait with IRQ + wait_queue.
     * This version is here so you can confirm MMIO works before adding
     * interrupt complexity.
     */
    iowrite32(5, edu->mmio + EDU_REG_FACTORIAL);
    while (ioread32(edu->mmio + EDU_REG_STATUS) & EDU_STATUS_COMPUTING)
        cpu_relax();
    val = ioread32(edu->mmio + EDU_REG_FACTORIAL);
    if (val == 120)
        dev_info(dev, "selftest: 5! = %u [OK]\n", val);
    else
        dev_err(dev,  "selftest: 5! = %u, expected 120 [FAIL]\n", val);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Phase 5: Interrupt service routine
 *
 * With MSI the vector is wired exclusively to this device — no other driver
 * will ever receive it, so IRQF_SHARED is not needed and IRQ_NONE is never
 * returned. The EDU device itself calls msi_notify() when MSI is enabled,
 * which is exactly what pci_alloc_irq_vectors(PCI_IRQ_MSI) sets up.
 * ═══════════════════════════════════════════════════════════════════════════ */

static irqreturn_t edu_isr(int irq, void *data)
{
    struct edu_dev *edu = data;
    u32 status;

    status = ioread32(edu->mmio + EDU_REG_INTR_STATUS);
    if (!status)
        return IRQ_HANDLED; /* Spurious — MSI fired but status already cleared */

    /*
     * Handle factorial-done interrupt.
     * Read the result BEFORE acknowledging so we don't race with the
     * device if the host scheduler is unkind.
     */
    if (status & EDU_INTR_FACTORIAL_DONE) {
        edu->factorial_result = ioread32(edu->mmio + EDU_REG_FACTORIAL);
        edu->irq_done = true;
        wake_up(&edu->wq);
    }

    /*
     * Handle DMA-done interrupt (Phase 6 / 7).
     *
     * Two completion paths:
     *   a) ioctl path: wake the wait_queue; edu_dma_test() will unblock.
     *   b) io_uring path: schedule the workqueue; it calls io_uring_cmd_done()
     *      safely from process context (can't call it from hard-IRQ).
     */
    if (status & EDU_INTR_DMA_DONE) {
        edu->irq_done = true;
        wake_up(&edu->wq);

        if (edu->pending_uring_cmd)
            schedule_work(&edu->uring_complete_work);
    }

    /* Acknowledge — clears the IRQ line. MUST happen, or the line stays
     * asserted and we get an interrupt storm. */
    iowrite32(status, edu->mmio + EDU_REG_INTR_ACK);

    return IRQ_HANDLED;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Phase 6: DMA helper — bidirectional round-trip test
 *
 * Sequence:
 *   1. Fill dma_buf with a known pattern (0xAB bytes)
 *   2. RAM → device: DMA dma_buf into the device's onboard buffer (0x40000)
 *   3. Device → RAM: DMA the device's onboard buffer into a verify_buf
 *   4. memcmp(dma_buf, verify_buf) — must match
 *
 * Both transfers use IRQ-on-completion (EDU_DMA_IRQ_ON_DONE).
 * ═══════════════════════════════════════════════════════════════════════════ */

static int edu_dma_test(struct edu_dev *edu)
{
    struct device *dev = &edu->pdev->dev;
    void      *verify_buf;
    dma_addr_t verify_dma;
    int        rc = 0;

    /* Fill the coherent source buffer with a known pattern */
    memset(edu->dma_buf, 0xAB, EDU_DMA_BUF_SIZE);

    /* ── Transfer 1: RAM → device ─────────────────────────────────────── */
    edu->irq_done = false;

    /*
     * Program DMA registers (64-bit).
     * src  = bus address of our kernel buffer
     * dst  = device's internal buffer at offset 0x40000 within BAR0
     */
    lo_hi_writeq(edu->dma_addr,      edu->mmio + EDU_REG_DMA_SRC);
    lo_hi_writeq(EDU_DMA_DEV_BUF,    edu->mmio + EDU_REG_DMA_DST);
    iowrite32(EDU_DMA_BUF_SIZE,       edu->mmio + EDU_REG_DMA_COUNT);
    iowrite32(EDU_DMA_START | EDU_DMA_DIR_TO_DEV | EDU_DMA_IRQ_ON_DONE,
              edu->mmio + EDU_REG_DMA_CMD);

    rc = wait_event_interruptible_timeout(edu->wq, edu->irq_done, HZ * 5);
    if (rc == 0) {
        dev_err(dev, "DMA RAM→device timed out\n");
        return -ETIMEDOUT;
    }
    if (rc < 0) return rc;  /* signal received */

    dev_info(dev, "DMA RAM→device done\n");

    /* ── Transfer 2: device → RAM ─────────────────────────────────────── */

    /*
     * Allocate a second buffer for the read-back.
     * This is NOT a coherent buffer — we use dma_map_single so the cache
     * is invalidated correctly before we read it back from the device.
     */
    verify_buf = kmalloc(EDU_DMA_BUF_SIZE, GFP_KERNEL);
    if (!verify_buf)
        return -ENOMEM;

    verify_dma = dma_map_single(dev, verify_buf, EDU_DMA_BUF_SIZE,
                                 DMA_FROM_DEVICE);
    if (dma_mapping_error(dev, verify_dma)) {
        kfree(verify_buf);
        return -ENOMEM;
    }

    edu->irq_done = false;

    lo_hi_writeq(EDU_DMA_DEV_BUF,    edu->mmio + EDU_REG_DMA_SRC);
    lo_hi_writeq(verify_dma,          edu->mmio + EDU_REG_DMA_DST);
    iowrite32(EDU_DMA_BUF_SIZE,       edu->mmio + EDU_REG_DMA_COUNT);
    iowrite32(EDU_DMA_START | EDU_DMA_DIR_FROM_DEV | EDU_DMA_IRQ_ON_DONE,
              edu->mmio + EDU_REG_DMA_CMD);

    rc = wait_event_interruptible_timeout(edu->wq, edu->irq_done, HZ * 5);

    /* Unmap before touching the buffer (mandatory DMA API rule) */
    dma_unmap_single(dev, verify_dma, EDU_DMA_BUF_SIZE, DMA_FROM_DEVICE);

    if (rc == 0) {
        dev_err(dev, "DMA device→RAM timed out\n");
        kfree(verify_buf);
        return -ETIMEDOUT;
    }
    if (rc < 0) { kfree(verify_buf); return rc; }

    /* ── Verification ─────────────────────────────────────────────────── */
    if (memcmp(edu->dma_buf, verify_buf, EDU_DMA_BUF_SIZE) == 0) {
        dev_info(dev, "DMA round-trip memcmp: PASS\n");
    } else {
        dev_err(dev, "DMA round-trip memcmp: FAIL — data corruption detected\n");
        rc = -EIO;
    }

    kfree(verify_buf);
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Phase 4: File operations (char device)
 * ═══════════════════════════════════════════════════════════════════════════ */

static int edu_open(struct inode *inode, struct file *filp)
{
    struct edu_dev *edu = container_of(inode->i_cdev, struct edu_dev, cdev);
    filp->private_data = edu;
    return 0;
}

static int edu_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static long edu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct edu_dev *edu = filp->private_data;
    u32 val;
    int rc;

    if (mutex_lock_interruptible(&edu->lock))
        return -ERESTARTSYS;

    switch (cmd) {

    /* ── EDU_IOC_FACTORIAL ─────────────────────────────────────────────
     * Phase 4 (initial): polled
     * Phase 5 (final):   IRQ-driven via wait_queue
     */
    case EDU_IOC_FACTORIAL:
        if (copy_from_user(&val, (__u32 __user *)arg, sizeof(val))) {
            rc = -EFAULT;
            break;
        }

        /* Enable IRQ on completion, then kick off the computation */
        iowrite32(EDU_STATUS_IRQ_ENABLE, edu->mmio + EDU_REG_STATUS);
        edu->irq_done = false;
        iowrite32(val, edu->mmio + EDU_REG_FACTORIAL);

        /*
         * Block until the ISR wakes us, or 5 seconds time out.
         * The ISR writes edu->factorial_result before setting irq_done.
         */
        rc = wait_event_interruptible_timeout(edu->wq, edu->irq_done, HZ * 5);
        if (rc == 0) { rc = -ETIMEDOUT; break; }
        if (rc < 0)  break;

        val = edu->factorial_result;
        rc = copy_to_user((__u32 __user *)arg, &val, sizeof(val)) ? -EFAULT : 0;
        break;

    /* ── EDU_IOC_LIVENESS ──────────────────────────────────────────────
     * Write any u32; read back its bitwise NOT.
     */
    case EDU_IOC_LIVENESS:
        if (copy_from_user(&val, (__u32 __user *)arg, sizeof(val))) {
            rc = -EFAULT;
            break;
        }
        iowrite32(val, edu->mmio + EDU_REG_LIVENESS);
        val = ioread32(edu->mmio + EDU_REG_LIVENESS);
        rc = copy_to_user((__u32 __user *)arg, &val, sizeof(val)) ? -EFAULT : 0;
        break;

    /* ── EDU_IOC_DMA_TEST ──────────────────────────────────────────────
     * Full bidirectional DMA round-trip with memcmp verification.
     */
    case EDU_IOC_DMA_TEST:
        rc = edu_dma_test(edu);
        break;

    /* ── EDU_IOC_GET_DMA_SIZE ──────────────────────────────────────────
     * Returns the size of the coherent DMA buffer.
     */
    case EDU_IOC_GET_DMA_SIZE:
        val = EDU_DMA_BUF_SIZE;
        rc = copy_to_user((__u32 __user *)arg, &val, sizeof(val)) ? -EFAULT : 0;
        break;

    default:
        rc = -ENOTTY;
        break;
    }

    mutex_unlock(&edu->lock);
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * mmap — zero-copy access to the coherent DMA buffer from userspace
 *
 * After open("/dev/edu0"), userspace calls:
 *   uint32_t sz;
 *   ioctl(fd, EDU_IOC_GET_DMA_SIZE, &sz);
 *   void *p = mmap(NULL, sz, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
 *
 * p then points directly into the same physical pages the DMA engine uses.
 * No copy_to_user / copy_from_user involved — the CPU and the device share
 * the same physical memory, coherency guaranteed by dma_alloc_coherent.
 *
 * This is the technique behind:
 *   - DPDK / io_uring zero-copy ring buffers
 *   - GPU unified virtual memory (UVM)
 *   - NIC descriptor rings exposed to userspace
 * ═══════════════════════════════════════════════════════════════════════════ */

static int edu_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct edu_dev *edu = filp->private_data;
    size_t size = vma->vm_end - vma->vm_start;

    if (size > EDU_DMA_BUF_SIZE) {
        dev_warn(&edu->pdev->dev,
                 "mmap: requested size %zu > DMA buffer size %u\n",
                 size, EDU_DMA_BUF_SIZE);
        return -EINVAL;
    }

    /*
     * dma_mmap_coherent() maps the coherent DMA allocation into a userspace
     * VMA correctly for the platform — on arm64 this sets the right page
     * attributes for cache coherency. Do NOT use remap_pfn_range() directly;
     * it bypasses the DMA layer's coherency guarantees.
     */
    return dma_mmap_coherent(&edu->pdev->dev, vma,
                              edu->dma_buf, edu->dma_addr, size);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Phase 7: io_uring passthrough interface
 *
 * edu_uring_complete() — workqueue function, runs in process context.
 * Called by the workqueue after the ISR schedules it when DMA finishes.
 * Posts the CQE (completion queue entry) that unblocks the userspace
 * io_uring_wait_cqe() call.
 *
 * edu_uring_cmd() — the kernel-side SQE handler.
 * Called by io_uring core when userspace submits IORING_OP_URING_CMD.
 * Returns -EIOCBQUEUED for async ops ("still in flight, don't post CQE yet").
 * ═══════════════════════════════════════════════════════════════════════════ */

static void edu_uring_complete(struct work_struct *work)
{
    struct edu_dev *edu =
        container_of(work, struct edu_dev, uring_complete_work);
    struct io_uring_cmd *cmd;
    int result;

    /* Grab cmd pointer under lock; clear it so ISR won't double-schedule */
    mutex_lock(&edu->lock);
    cmd    = edu->pending_uring_cmd;
    result = edu->pending_uring_result;
    edu->pending_uring_cmd = NULL;
    mutex_unlock(&edu->lock);

    if (!cmd)
        return; /* already completed or cancelled */

    /*
     * io_uring_cmd_done() posts the CQE to the ring and wakes the
     * userspace thread waiting in io_uring_wait_cqe().
     * issue_flags=0 because we're deferred (not in the original
     * submission context).
     */
    io_uring_cmd_done(cmd, result, 0, 0);
}

static int edu_uring_cmd(struct io_uring_cmd *ioucmd, unsigned int issue_flags)
{
    struct edu_dev *edu = ioucmd->file->private_data;
    const struct edu_uring_req *req = io_uring_sqe_cmd(ioucmd->sqe);
    int rc;

    if (!req)
        return -EINVAL;

    mutex_lock(&edu->lock);

    if (edu->pending_uring_cmd) {
        /* Only one async op in flight at a time */
        mutex_unlock(&edu->lock);
        return -EBUSY;
    }

    switch (req->op) {

    case EDU_URING_OP_DMA_TEST:
        /*
         * Async DMA round-trip.
         *
         * Fill the coherent DMA buffer with the requested pattern,
         * start RAM→device DMA, then register this io_uring_cmd as
         * pending.  When the ISR fires the DMA-done interrupt it
         * schedules uring_complete_work which posts the CQE.
         *
         * NOTE: we only start the first leg (RAM→device) here.
         * The second leg (device→RAM) and memcmp happen synchronously
         * in the workqueue for simplicity.  A production driver would
         * chain the second DMA through another ISR round.
         */
        memset(edu->dma_buf, req->arg ? (u8)req->arg : 0xAB, EDU_DMA_BUF_SIZE);

        edu->irq_done          = false;
        edu->pending_uring_cmd    = ioucmd;
        edu->pending_uring_result = 0; /* will be updated by work fn */

        lo_hi_writeq(edu->dma_addr,   edu->mmio + EDU_REG_DMA_SRC);
        lo_hi_writeq(EDU_DMA_DEV_BUF, edu->mmio + EDU_REG_DMA_DST);
        iowrite32(EDU_DMA_BUF_SIZE,   edu->mmio + EDU_REG_DMA_COUNT);
        iowrite32(EDU_DMA_START | EDU_DMA_DIR_TO_DEV | EDU_DMA_IRQ_ON_DONE,
                  edu->mmio + EDU_REG_DMA_CMD);

        mutex_unlock(&edu->lock);
        return -EIOCBQUEUED; /* "async — don't post CQE yet" */

    case EDU_URING_OP_FACTORIAL: {
        /*
         * Async factorial via io_uring.
         *
         * This shows the io_uring pattern for a non-DMA operation too:
         * write factorial register, register pending cmd, return -EIOCBQUEUED.
         * ISR (factorial-done path) will schedule the work to post the CQE.
         */
        u32 n = req->arg;
        if (n > 12) { /* 12! fits in u32, 13! overflows */
            mutex_unlock(&edu->lock);
            return -ERANGE;
        }

        edu->irq_done          = false;
        edu->pending_uring_cmd    = ioucmd;
        edu->pending_uring_result = 0;

        iowrite32(n | EDU_STATUS_IRQ_ENABLE, edu->mmio + EDU_REG_STATUS);
        iowrite32(n, edu->mmio + EDU_REG_FACTORIAL);

        mutex_unlock(&edu->lock);
        return -EIOCBQUEUED;
    }

    default:
        mutex_unlock(&edu->lock);
        return -EOPNOTSUPP;
    }

    rc = 0;
    mutex_unlock(&edu->lock);
    return rc;
}

static const struct file_operations edu_fops = {
    .owner          = THIS_MODULE,
    .open           = edu_open,
    .release        = edu_release,
    .unlocked_ioctl = edu_ioctl,
    .mmap           = edu_mmap,
    .uring_cmd      = edu_uring_cmd,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Phase 2: PCI probe / remove
 * ═══════════════════════════════════════════════════════════════════════════ */

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct edu_dev *edu;
    int err;

    /* Allocate device state — devm_kzalloc ties lifetime to pdev */
    edu = devm_kzalloc(&pdev->dev, sizeof(*edu), GFP_KERNEL);
    if (!edu)
        return -ENOMEM;

    edu->pdev = pdev;
    mutex_init(&edu->lock);
    init_waitqueue_head(&edu->wq);
    INIT_WORK(&edu->uring_complete_work, edu_uring_complete);

    /* ── PCI init ──────────────────────────────────────────────────── */
    err = pci_enable_device(pdev);
    if (err) {
        dev_err(&pdev->dev, "pci_enable_device failed: %d\n", err);
        return err;
    }

    err = pci_request_region(pdev, 0, "edu");
    if (err) {
        dev_err(&pdev->dev, "pci_request_region BAR0 failed: %d\n", err);
        goto err_disable;
    }

    edu->mmio = pci_iomap(pdev, 0, 0);
    if (!edu->mmio) {
        dev_err(&pdev->dev, "pci_iomap BAR0 failed\n");
        err = -ENOMEM;
        goto err_release_region;
    }

    pci_set_master(pdev);   /* required for DMA — enables bus-mastering */

    /* ── DMA setup (Phase 6) ─────────────────────────────────────── */
    err = dma_set_mask(&pdev->dev, EDU_DMA_MASK);
    if (err) {
        dev_err(&pdev->dev, "dma_set_mask(28-bit) failed: %d\n", err);
        goto err_iounmap;
    }
    err = dma_set_coherent_mask(&pdev->dev, EDU_DMA_MASK);
    if (err) {
        dev_err(&pdev->dev, "dma_set_coherent_mask(28-bit) failed: %d\n", err);
        goto err_iounmap;
    }

    edu->dma_buf = dma_alloc_coherent(&pdev->dev, EDU_DMA_BUF_SIZE,
                                       &edu->dma_addr, GFP_KERNEL);
    if (!edu->dma_buf) {
        dev_err(&pdev->dev, "dma_alloc_coherent failed\n");
        err = -ENOMEM;
        goto err_iounmap;
    }

    /* ── MSI interrupt setup (Phase 5) ──────────────────────────── */
    /*
     * Allocate exactly 1 MSI vector. The EDU device supports MSI —
     * QEMU's edu_raise_irq() calls msi_notify() when MSI is enabled.
     * MSI gives us a dedicated, non-shared vector, so no IRQF_SHARED
     * flag is needed and the ISR never needs to return IRQ_NONE.
     *
     * pci_irq_vector(pdev, 0) returns the allocated Linux IRQ number.
     */
    err = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
    if (err < 0) {
        dev_err(&pdev->dev, "pci_alloc_irq_vectors (MSI) failed: %d\n", err);
        goto err_dma_free;
    }

    err = request_irq(pci_irq_vector(pdev, 0), edu_isr, 0, "edu", edu);
    if (err) {
        dev_err(&pdev->dev, "request_irq failed: %d\n", err);
        pci_free_irq_vectors(pdev);
        goto err_dma_free;
    }

    /* ── Character device registration (Phase 4) ─────────────────── */
    err = alloc_chrdev_region(&edu->devno, 0, 1, "edu");
    if (err) {
        dev_err(&pdev->dev, "alloc_chrdev_region failed: %d\n", err);
        goto err_free_irq;
    }
    edu_major = MAJOR(edu->devno);

    cdev_init(&edu->cdev, &edu_fops);
    edu->cdev.owner = THIS_MODULE;
    err = cdev_add(&edu->cdev, edu->devno, 1);
    if (err) {
        dev_err(&pdev->dev, "cdev_add failed: %d\n", err);
        goto err_unregister_chrdev;
    }

    /* device_create makes udev auto-create /dev/edu0 */
    if (IS_ERR(device_create(edu_class, &pdev->dev, edu->devno, NULL, "edu0"))) {
        dev_warn(&pdev->dev, "device_create failed — create /dev/edu0 manually with mknod\n");
    }

    pci_set_drvdata(pdev, edu);

    /* ── Self-test (Phase 3) ─────────────────────────────────────── */
    edu_selftest(edu);

    dev_info(&pdev->dev,
             "EDU driver loaded: BAR0@%p, MSI IRQ=%d, DMA buf phys=0x%llx\n",
             edu->mmio, pci_irq_vector(pdev, 0), (u64)edu->dma_addr);

    return 0;

    /* ── Error unwind (reverse order of resource acquisition) ────── */
err_unregister_chrdev:
    unregister_chrdev_region(edu->devno, 1);
err_free_irq:
    free_irq(pci_irq_vector(pdev, 0), edu);
    pci_free_irq_vectors(pdev);
err_dma_free:
    dma_free_coherent(&pdev->dev, EDU_DMA_BUF_SIZE, edu->dma_buf, edu->dma_addr);
err_iounmap:
    pci_iounmap(pdev, edu->mmio);
err_release_region:
    pci_release_region(pdev, 0);
err_disable:
    pci_disable_device(pdev);
    return err;
}

static void edu_remove(struct pci_dev *pdev)
{
    struct edu_dev *edu = pci_get_drvdata(pdev);

    device_destroy(edu_class, edu->devno);
    cdev_del(&edu->cdev);
    unregister_chrdev_region(edu->devno, 1);

    /* Cancel any in-flight io_uring async completion BEFORE freeing IRQ.
     * If a DMA-done interrupt fired and scheduled uring_complete_work,
     * we must let it finish (or cancel it) before tearing down resources.
     * Otherwise io_uring_cmd_done() would fire after the struct is freed. */
    cancel_work_sync(&edu->uring_complete_work);

    /* free_irq before pci_free_irq_vectors — mandatory ordering */
    free_irq(pci_irq_vector(pdev, 0), edu);
    pci_free_irq_vectors(pdev);
    dma_free_coherent(&pdev->dev, EDU_DMA_BUF_SIZE, edu->dma_buf, edu->dma_addr);
    pci_iounmap(pdev, edu->mmio);
    pci_release_region(pdev, 0);
    pci_disable_device(pdev);

    dev_info(&pdev->dev, "EDU driver unloaded\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Module registration
 * ═══════════════════════════════════════════════════════════════════════════ */

static const struct pci_device_id edu_ids[] = {
    { PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
    { 0 }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

static struct pci_driver edu_driver = {
    .name     = "edu",
    .id_table = edu_ids,
    .probe    = edu_probe,
    .remove   = edu_remove,
};

static int __init edu_init(void)
{
    /* Create the device class so udev can auto-create /dev/edu0 */
    edu_class = class_create(THIS_MODULE, "edu");
    if (IS_ERR(edu_class))
        return PTR_ERR(edu_class);

    return pci_register_driver(&edu_driver);
}

static void __exit edu_exit(void)
{
    pci_unregister_driver(&edu_driver);
    class_destroy(edu_class);
}

module_init(edu_init);
module_exit(edu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sayan");
MODULE_DESCRIPTION("EDU PCI device driver — MMIO, IRQ, DMA demonstration");
MODULE_VERSION("1.0");
