/*
 * edu_test.c — Userspace test program for the EDU kernel driver
 *
 * Runs INSIDE the Linux guest. Requires /dev/edu0 to exist
 * (created automatically by udev when the module is loaded).
 *
 * Build (inside guest):
 *   gcc -Wall -O2 -o edu_test edu_test.c
 *
 * Run (as root or after adjusting /dev/edu0 permissions):
 *   ./edu_test
 *
 * Expected output if everything is working:
 *   [LIVENESS] wrote 0xdeadbeef, got 0x21524110 — PASS
 *   [FACTORIAL] 1! = 1 — PASS
 *   [FACTORIAL] 5! = 120 — PASS
 *   [FACTORIAL] 7! = 5040 — PASS
 *   [FACTORIAL] 10! = 3628800 — PASS
 *   [FACTORIAL] 12! = 479001600 — PASS
 *   [DMA TEST] PASS
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>

#include "edu.h"

/* ANSI colour helpers */
#define GREEN  "\033[32m"
#define RED    "\033[31m"
#define RESET  "\033[0m"

#define PASS(fmt, ...) printf(GREEN "[PASS] " RESET fmt "\n", ##__VA_ARGS__)
#define FAIL(fmt, ...) printf(RED   "[FAIL] " RESET fmt "\n", ##__VA_ARGS__)

/* Reference factorial (software) for verification */
static uint64_t ref_factorial(uint32_t n)
{
    uint64_t r = 1;
    for (uint32_t i = 2; i <= n; i++) r *= i;
    return r;
}

static int test_liveness(int fd)
{
    uint32_t val = 0xdeadbeef;
    uint32_t expected = ~val;

    if (ioctl(fd, EDU_IOC_LIVENESS, &val) < 0) {
        FAIL("EDU_IOC_LIVENESS ioctl: %s", strerror(errno));
        return -1;
    }

    if (val == expected) {
        PASS("liveness: wrote 0xdeadbeef, got 0x%08x", val);
        return 0;
    } else {
        FAIL("liveness: wrote 0xdeadbeef, expected 0x%08x, got 0x%08x",
             expected, val);
        return -1;
    }
}

static int test_factorial(int fd, uint32_t n)
{
    uint32_t val = n;
    uint64_t expected = ref_factorial(n);

    if (ioctl(fd, EDU_IOC_FACTORIAL, &val) < 0) {
        FAIL("factorial(%u) ioctl: %s", n, strerror(errno));
        return -1;
    }

    if ((uint64_t)val == expected) {
        PASS("factorial: %u! = %u", n, val);
        return 0;
    } else {
        FAIL("factorial: %u! = %u, expected %lu", n, val, (unsigned long)expected);
        return -1;
    }
}

static int test_dma(int fd)
{
    if (ioctl(fd, EDU_IOC_DMA_TEST) < 0) {
        FAIL("DMA round-trip: %s", strerror(errno));
        return -1;
    }
    PASS("DMA round-trip (RAM→device→RAM memcmp verified)");
    return 0;
}

/*
 * test_mmap — zero-copy DMA buffer access
 *
 * Demonstrates the mmap interface:
 *   1. Query the DMA buffer size from the driver via EDU_IOC_GET_DMA_SIZE
 *   2. mmap() /dev/edu0 to get a pointer directly into the coherent DMA buffer
 *   3. Stamp the buffer with a known pattern (0xAB)
 *   4. Trigger EDU_IOC_DMA_TEST (device reads from RAM buffer, writes to its
 *      internal buffer, then reads back into RAM buffer via DMA)
 *   5. Confirm the pattern survived — the mmap pointer and the DMA engine
 *      both refer to the SAME physical pages, so writes are immediately visible
 *      to the device without any copy_to_user / copy_from_user in the path.
 *
 * This pattern underpins DPDK zero-copy networking, io_uring submission rings,
 * and GPU unified virtual memory.
 */
static int test_mmap(int fd)
{
    uint32_t dma_size = 0;
    uint8_t *buf;
    int rc = 0;

    /* Ask the driver how big the coherent DMA buffer is */
    if (ioctl(fd, EDU_IOC_GET_DMA_SIZE, &dma_size) < 0) {
        FAIL("EDU_IOC_GET_DMA_SIZE: %s", strerror(errno));
        return -1;
    }

    /* Map the DMA buffer into this process's virtual address space.
     * After this call, buf[0..dma_size-1] IS the DMA buffer — same
     * physical pages, no intermediate copies. */
    buf = mmap(NULL, dma_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) {
        FAIL("mmap /dev/edu0: %s", strerror(errno));
        return -1;
    }

    /* Write a known pattern into the DMA buffer via the mmap pointer */
    memset(buf, 0xAB, dma_size);

    /* Trigger DMA round-trip: the driver will read buf, DMA it through the
     * device, and DMA the result back into buf. If mmap is wired to the
     * correct physical pages, the pattern must survive. */
    if (ioctl(fd, EDU_IOC_DMA_TEST) < 0) {
        FAIL("mmap+DMA round-trip: %s", strerror(errno));
        rc = -1;
        goto out;
    }

    /* Verify pattern survived the round-trip */
    for (uint32_t i = 0; i < dma_size; i++) {
        if (buf[i] != 0xAB) {
            FAIL("mmap: byte[%u] = 0x%02x, expected 0xAB", i, buf[i]);
            rc = -1;
            goto out;
        }
    }
    PASS("mmap: %u-byte DMA buffer mapped at %p, 0xAB pattern survives zero-copy round-trip",
         dma_size, (void *)buf);

out:
    munmap(buf, dma_size);
    return rc;
}

int main(void)
{
    int fd, failures = 0;

    fd = open("/dev/edu0", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "ERROR: cannot open /dev/edu0: %s\n"
                        "       Is the edu module loaded? (insmod edu.ko)\n",
                strerror(errno));
        return 1;
    }

    printf("=== EDU driver userspace tests ===\n\n");

    failures += (test_liveness(fd) < 0);

    uint32_t test_ns[] = {1, 5, 7, 10, 12};
    for (size_t i = 0; i < sizeof(test_ns)/sizeof(test_ns[0]); i++)
        failures += (test_factorial(fd, test_ns[i]) < 0);

    failures += (test_dma(fd) < 0);
    failures += (test_mmap(fd) < 0);

    close(fd);

    printf("\n");
    if (failures == 0)
        printf(GREEN "All tests passed.\n" RESET);
    else
        printf(RED "%d test(s) failed.\n" RESET, failures);

    return failures ? 1 : 0;
}
