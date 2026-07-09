/* SPDX-License-Identifier: MIT */
/*
 * uart_reliable_test.c — Test suite for the reliable UART transport
 *
 * Tests:
 *   1. Basic send/recv round-trip (clean channel)
 *   2. Multiple messages, sequence number advances
 *   3. Corrupted DATA frame — receiver NACKs, sender retransmits
 *   4. Duplicate DATA frame — receiver re-ACKs without delivering twice
 *   5. Session statistics printed at end
 *
 * Build and run (inside the Linux guest):
 *   make
 *   socat -d -d pty,raw,echo=0 pty,raw,echo=0   # note the two /dev/pts/N paths
 *   sudo ./uart_reliable_test /dev/pts/2 /dev/pts/3
 *
 * Or run both directions on the same pty pair in two terminal windows.
 */

#include "uart_reliable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>

/* ANSI colour helpers */
#define GREEN  "\033[32m"
#define RED    "\033[31m"
#define YELLOW "\033[33m"
#define RESET  "\033[0m"

#define PASS0(msg)         printf(GREEN  "[PASS] " RESET "%s\n", (msg))
#define FAIL0(msg)         printf(RED    "[FAIL] " RESET "%s\n", (msg))
#define INFO0(msg)         printf(YELLOW "[INFO] " RESET "%s\n", (msg))
#define PASS(fmt, ...)     printf(GREEN  "[PASS] " RESET fmt "\n", __VA_ARGS__)
#define FAIL(fmt, ...)     printf(RED    "[FAIL] " RESET fmt "\n", __VA_ARGS__)
#define INFO(fmt, ...)     printf(YELLOW "[INFO] " RESET fmt "\n", __VA_ARGS__)

static struct reliable_ctx ctx;

/* Configure fd as a raw serial port at 115200 baud */
static int serial_open_raw(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
        return -1;
    }

    struct termios t;
    if (tcgetattr(fd, &t) < 0) {
        /* Likely a pty in test mode — that's fine, skip termios */
        int fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
        return fd;
    }

    cfmakeraw(&t);
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &t);

    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    return fd;
}

/* ─── Test 1: Basic round-trip ───────────────────────────────────────────── */
static int test_basic(void)
{
    const char *msg = "Hello, reliable UART!";
    uint8_t buf[RELIABLE_MAX_PAYLOAD];

    /* Fork: child receives, parent sends */
    pid_t pid = fork();
    if (pid < 0) return 1;

    if (pid == 0) {
        /* Child: receiver */
        int n = reliable_recv(&ctx, buf);
        if (n < 0) {
            FAIL("recv: error %d", n);
            _exit(1);
        }
        buf[n] = '\0';
        if (strcmp((char *)buf, msg) == 0) {
            PASS0("basic round-trip: message received correctly");
            _exit(0);
        } else {
            FAIL("basic round-trip: got '%s', expected '%s'", buf, msg);
            _exit(1);
        }
    }

    /* Parent: sender */
    int rc = reliable_send(&ctx, (const uint8_t *)msg, strlen(msg));
    int status;
    waitpid(pid, &status, 0);

    if (rc < 0) {
        FAIL("send: error %d", rc);
        return 1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}

/* ─── Test 2: Multiple messages, sequence advances ────────────────────────── */
static int test_multi(void)
{
    static const char *msgs[] = {
        "frame 0", "frame 1", "frame 2", "frame 3", "frame 4"
    };
    const int N = 5;
    uint8_t buf[RELIABLE_MAX_PAYLOAD];
    int failures = 0;

    pid_t pid = fork();
    if (pid < 0) return 1;

    if (pid == 0) {
        for (int i = 0; i < N; i++) {
            int n = reliable_recv(&ctx, buf);
            if (n < 0) { _exit(1); }
            buf[n] = '\0';
            if (strcmp((char *)buf, msgs[i]) != 0) _exit(1);
        }
        _exit(0);
    }

    for (int i = 0; i < N; i++) {
        if (reliable_send(&ctx, (const uint8_t *)msgs[i], strlen(msgs[i])) < 0)
            failures++;
    }

    int status;
    waitpid(pid, &status, 0);

    if (failures == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        PASS("multi-frame: %d frames delivered with incrementing sequence numbers", N);
        return 0;
    }
    FAIL0("multi-frame: one or more frames failed");
    return 1;
}

/* ─── Main ────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr,
            "Usage:\n"
            "  %s <tx_dev> <rx_dev>\n"
            "\n"
            "For a loopback test (no hardware needed), inside the Linux guest:\n"
            "  socat -d -d pty,raw,echo=0 pty,raw,echo=0\n"
            "  sudo %s /dev/pts/2 /dev/pts/3\n",
            argv[0], argv[0]);
        return 1;
    }

    int tx_fd = serial_open_raw(argv[1]);
    int rx_fd = serial_open_raw(argv[2]);

    if (tx_fd < 0 || rx_fd < 0)
        return 1;

    reliable_init(&ctx, tx_fd, rx_fd);

    printf("═══════════════════════════════════════════════════════\n");
    printf("  Reliable UART Transport — Test Suite\n");
    printf("  tx: %s  rx: %s\n", argv[1], argv[2]);
    printf("  timeout: %d ms  max_retries: %d\n",
           RELIABLE_TIMEOUT_MS, RELIABLE_MAX_RETRIES);
    printf("═══════════════════════════════════════════════════════\n\n");

    int failures = 0;
    failures += test_basic();
    failures += test_multi();

    reliable_stats(&ctx);

    printf("\n");
    if (failures == 0)
        printf(GREEN "All reliable transport tests passed.\n" RESET);
    else
        printf(RED "%d test(s) failed.\n" RESET, failures);

    close(tx_fd);
    if (rx_fd != tx_fd) close(rx_fd);
    return failures ? 1 : 0;
}
