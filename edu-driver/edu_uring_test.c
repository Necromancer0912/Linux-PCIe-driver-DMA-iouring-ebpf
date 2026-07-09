/* SPDX-License-Identifier: MIT */
/*
 * edu_uring_test.c — io_uring async interface test for the EDU driver
 *
 * Demonstrates the difference between:
 *   ioctl  = synchronous (thread blocks in kernel until done)
 *   io_uring = asynchronous (thread submits, continues working,
 *               gets notified when hardware is done)
 *
 * Requires inside the guest:
 *   apt install liburing-dev
 *   gcc -Wall -O2 -o edu_uring_test edu_uring_test.c -luring
 *   sudo ./edu_uring_test
 *
 * What to look for in dmesg after running:
 *   "EDU DMA RAM→device done"     ← ISR fired, workqueue scheduled
 *   The CQE arrives in userspace without a blocking syscall — the
 *   thread was free to do other work (we print a message) while DMA ran.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>

/* liburing — install with: apt install liburing-dev */
#include <liburing.h>

#include "edu.h"

/* ANSI colour helpers */
#define GREEN  "\033[32m"
#define RED    "\033[31m"
#define CYAN   "\033[36m"
#define YELLOW "\033[33m"
#define RESET  "\033[0m"

#define PASS(fmt, ...) printf(GREEN  "[PASS] " RESET fmt "\n", ##__VA_ARGS__)
#define FAIL(fmt, ...) printf(RED    "[FAIL] " RESET fmt "\n", ##__VA_ARGS__)
#define INFO(fmt, ...) printf(CYAN   "[INFO] " RESET fmt "\n", ##__VA_ARGS__)
#define NOTE(fmt, ...) printf(YELLOW "[NOTE] " RESET fmt "\n", ##__VA_ARGS__)

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ─── Test 1: Async DMA via io_uring ─────────────────────────────────────── *
 *
 * Flow:
 *  1. Build an SQE with IORING_OP_URING_CMD + our edu_uring_req payload
 *  2. Submit to ring → kernel calls edu_uring_cmd() → DMA starts → -EIOCBQUEUED
 *  3. While DMA is running IN HARDWARE, we do useful work in userspace
 *  4. Eventually call io_uring_wait_cqe() to collect the result
 *  5. Check cqe->res == 0 (success)
 *
 * Compare to the ioctl path: step 2 would block the thread until DMA finishes.
 * ─────────────────────────────────────────────────────────────────────────── */
static int test_async_dma(int fd)
{
    struct io_uring ring;
    int rc;

    /* Initialise a ring with 8 entries — more than enough for this test */
    rc = io_uring_queue_init(8, &ring, 0);
    if (rc < 0) {
        FAIL("io_uring_queue_init: %s", strerror(-rc));
        return 1;
    }

    /* Build the request payload that the driver will read via io_uring_sqe_cmd() */
    struct edu_uring_req req = {
        .op  = EDU_URING_OP_DMA_TEST,
        .arg = 0xCD,    /* fill pattern for the DMA buffer */
    };

    /* Get a Submission Queue Entry */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        FAIL("io_uring_get_sqe: ring is full");
        io_uring_queue_exit(&ring);
        return 1;
    }

    /*
     * IORING_OP_URING_CMD is the "pass arbitrary command to device driver"
     * opcode added in kernel 5.19.  The driver's .uring_cmd handler receives
     * the full sqe so it can read our edu_uring_req from sqe->cmd[].
     */
    io_uring_prep_uring_cmd(sqe, fd, 0, 0);
    /* Copy our payload into the 80-byte SQE cmd area */
    memcpy((void *)sqe->cmd, &req, sizeof(req));
    io_uring_sqe_set_data(sqe, (void *)0xDEAD); /* tag for identification */

    uint64_t t_submit = now_ns();

    /* Submit to kernel — this starts the DMA immediately.
     * The kernel call returns quickly; DMA is running IN HARDWARE. */
    rc = io_uring_submit(&ring);
    if (rc < 0) {
        FAIL("io_uring_submit: %s", strerror(-rc));
        io_uring_queue_exit(&ring);
        return 1;
    }

    /*
     * *** KEY POINT: the thread is NOT blocked. ***
     * We can do useful computation here while the DMA engine is running.
     * In a real application this would be:
     *   - Processing already-received network packets
     *   - Computing on previously DMA'd data
     *   - Submitting MORE io_uring operations
     * We just print something to demonstrate the concept.
     */
    NOTE("DMA started — thread is FREE while hardware runs. Doing other work...");
    uint64_t busy_sum = 0;
    for (volatile int i = 0; i < 1000000; i++) busy_sum += i;
    NOTE("...finished 1M iterations (sum=%lu) while DMA was in flight.", busy_sum);

    /* Now collect the completion — blocks only until the hardware is done */
    struct io_uring_cqe *cqe;
    rc = io_uring_wait_cqe(&ring, &cqe);
    if (rc < 0) {
        FAIL("io_uring_wait_cqe: %s", strerror(-rc));
        io_uring_queue_exit(&ring);
        return 1;
    }

    uint64_t t_done = now_ns();
    uint64_t latency_us = (t_done - t_submit) / 1000;

    if (cqe->res != 0) {
        FAIL("async DMA: CQE result = %d (%s)", cqe->res, strerror(-cqe->res));
        io_uring_cqe_seen(&ring, cqe);
        io_uring_queue_exit(&ring);
        return 1;
    }

    PASS("async DMA via io_uring: CQE res=0, total latency=%lu µs", latency_us);
    io_uring_cqe_seen(&ring, cqe);
    io_uring_queue_exit(&ring);
    return 0;
}

/* ─── Test 2: Async factorial via io_uring ────────────────────────────────── */
static int test_async_factorial(int fd, uint32_t n, uint64_t expected)
{
    struct io_uring ring;
    int rc;

    rc = io_uring_queue_init(4, &ring, 0);
    if (rc < 0) { FAIL("ring init: %s", strerror(-rc)); return 1; }

    struct edu_uring_req req = {
        .op  = EDU_URING_OP_FACTORIAL,
        .arg = n,
    };

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_uring_cmd(sqe, fd, 0, 0);
    memcpy((void *)sqe->cmd, &req, sizeof(req));

    io_uring_submit(&ring);

    struct io_uring_cqe *cqe;
    rc = io_uring_wait_cqe(&ring, &cqe);
    if (rc < 0) { FAIL("wait_cqe: %s", strerror(-rc)); io_uring_queue_exit(&ring); return 1; }

    int result = cqe->res;
    io_uring_cqe_seen(&ring, cqe);
    io_uring_queue_exit(&ring);

    if (result < 0) {
        FAIL("async factorial(%u): kernel error %d (%s)", n, result, strerror(-result));
        return 1;
    }

    if ((uint64_t)(uint32_t)result == expected) {
        PASS("async factorial(%u) = %u via io_uring", n, (uint32_t)result);
        return 0;
    }
    FAIL("async factorial(%u): got %u, expected %lu", n, (uint32_t)result, expected);
    return 1;
}

/* ─── Test 3: Throughput — compare ioctl vs io_uring ─────────────────────── */
#define BENCH_ITERS 10

static int test_throughput_compare(int fd)
{
    uint64_t t0, t1;
    int failures = 0;

    /* ioctl path */
    t0 = now_ns();
    for (int i = 0; i < BENCH_ITERS; i++) {
        if (ioctl(fd, EDU_IOC_DMA_TEST) < 0) failures++;
    }
    t1 = now_ns();
    uint64_t ioctl_avg_us = (t1 - t0) / (BENCH_ITERS * 1000);

    /* io_uring path (submit all, then harvest all CQEs) */
    struct io_uring ring;
    io_uring_queue_init(BENCH_ITERS, &ring, 0);

    struct edu_uring_req req = { .op = EDU_URING_OP_DMA_TEST, .arg = 0xFF };

    t0 = now_ns();
    /*
     * With io_uring we can submit multiple ops before waiting for any CQE.
     * The hardware can theoretically pipeline them (though the edu device is
     * single-op, so they'll run sequentially here — the point is the reduced
     * syscall overhead).
     */
    for (int i = 0; i < BENCH_ITERS; i++) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        io_uring_prep_uring_cmd(sqe, fd, 0, 0);
        memcpy((void *)sqe->cmd, &req, sizeof(req));
    }
    io_uring_submit(&ring);

    for (int i = 0; i < BENCH_ITERS; i++) {
        struct io_uring_cqe *cqe;
        io_uring_wait_cqe(&ring, &cqe);
        if (cqe->res < 0) failures++;
        io_uring_cqe_seen(&ring, cqe);
    }
    t1 = now_ns();
    uint64_t uring_avg_us = (t1 - t0) / (BENCH_ITERS * 1000);
    io_uring_queue_exit(&ring);

    printf("\n┌─────────────────────────────────────────────────┐\n");
    printf("│  DMA throughput comparison (%d iterations each)  │\n", BENCH_ITERS);
    printf("├─────────────────────────────────────────────────┤\n");
    printf("│  ioctl:    avg latency = %5lu µs per op        │\n", ioctl_avg_us);
    printf("│  io_uring: avg latency = %5lu µs per op        │\n", uring_avg_us);
    if (uring_avg_us < ioctl_avg_us)
        printf("│  " GREEN "io_uring is faster (lower syscall overhead)" RESET "     │\n");
    else
        printf("│  " YELLOW "ioctl faster (DMA is the bottleneck here)" RESET "      │\n");
    printf("└─────────────────────────────────────────────────┘\n");

    return failures ? 1 : 0;
}

/* ─── Main ────────────────────────────────────────────────────────────────── */
int main(void)
{
    int fd = open("/dev/edu0", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "ERROR: cannot open /dev/edu0: %s\n"
                        "       Is the edu module loaded? (insmod edu.ko)\n",
                strerror(errno));
        return 1;
    }

    printf("═══════════════════════════════════════════════════════\n");
    printf("  EDU Driver — io_uring async interface tests\n");
    printf("═══════════════════════════════════════════════════════\n\n");

    int failures = 0;
    failures += test_async_dma(fd);
    failures += test_async_factorial(fd, 7,  5040);
    failures += test_async_factorial(fd, 10, 3628800);
    failures += test_throughput_compare(fd);

    close(fd);

    printf("\n");
    if (failures == 0)
        printf(GREEN "All io_uring tests passed.\n" RESET);
    else
        printf(RED "%d test(s) failed.\n" RESET, failures);
    return failures ? 1 : 0;
}
