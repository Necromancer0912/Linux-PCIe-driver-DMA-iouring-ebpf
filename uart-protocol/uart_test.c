/*
 * uart_test.c — Loopback test harness for the UART framing protocol
 *
 * Tests:
 *   1. Basic loopback: send N frames of known data, recv and verify each
 *   2. Byte-stuffing round-trip: payload containing 0x7E and 0x7D bytes
 *   3. Corruption detection: deliberately flip a bit in a captured frame,
 *      confirm checksum rejection + successful resync on next good frame
 *   4. Resync: inject garbage bytes before a valid frame, confirm recovery
 *
 * HARDWARE OPTION:
 *   Connect a USB-TTL adapter (CP2102 or FT232). Jumper TX pin to RX pin
 *   (loopback). macOS auto-detects it as /dev/tty.usbserial-XXXX.
 *   Usage: ./uart_test /dev/tty.usbserial-0001
 *
 * SOFTWARE OPTION (no hardware needed):
 *   Use socat to create a linked pseudoterminal pair:
 *     socat -d -d pty,raw,echo=0 pty,raw,echo=0
 *   Note the two /dev/ttys* paths it prints. Run in two terminals:
 *     ./uart_test /dev/ttys003 /dev/ttys004
 *   With two args, the first is used for TX, the second for RX.
 *   In loopback mode (one arg), TX and RX are the same fd.
 *
 * Build:
 *   cd uart-protocol && make
 *   # or manually: clang -Wall -Wextra -O2 -o uart_test uart_frame.c uart_test.c
 */

#include "uart_frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <termios.h>

/* ANSI colour helpers */
#define GREEN  "\033[32m"
#define RED    "\033[31m"
#define YELLOW "\033[33m"
#define RESET  "\033[0m"

/*
 * Portable logging helpers.
 * Two variants to avoid variadic-macro-with-no-args edge cases:
 *   PASS0/FAIL0/INFO0  — format string only, no extra arguments
 *   PASS/FAIL/INFO     — format string + one or more extra arguments
 */
#define PASS0(msg)         printf(GREEN  "[PASS] " RESET "%s\n", (msg))
#define FAIL0(msg)         printf(RED    "[FAIL] " RESET "%s\n", (msg))
#define INFO0(msg)         printf(YELLOW "[INFO] " RESET "%s\n", (msg))
#define PASS(fmt, ...)     printf(GREEN  "[PASS] " RESET fmt "\n", __VA_ARGS__)
#define FAIL(fmt, ...)     printf(RED    "[FAIL] " RESET fmt "\n", __VA_ARGS__)
#define INFO(fmt, ...)     printf(YELLOW "[INFO] " RESET fmt "\n", __VA_ARGS__)

static int tx_fd, rx_fd;

/* ─── Test 1: Basic loopback ────────────────────────────────────────────── */

static int test_basic_loopback(void)
{
    printf("\n--- Test 1: Basic loopback ---\n");

    const char *messages[] = {
        "Hello, kernel!",
        "PCIe driver",
        "UART framing",
        "aarch64 QEMU",
        "DMA verified",
    };
    int failures = 0;

    for (size_t i = 0; i < sizeof(messages)/sizeof(messages[0]); i++) {
        const uint8_t *payload = (const uint8_t *)messages[i];
        uint8_t  len = (uint8_t)strlen(messages[i]);
        uint8_t  rxbuf[FRAME_MAX_PAYLOAD];
        int      rxlen;

        if (send_frame(tx_fd, payload, len) < 0) {
            FAIL("send_frame(%s): I/O error", messages[i]);
            failures++;
            continue;
        }

        rxlen = recv_frame(rx_fd, rxbuf);
        if (rxlen < 0) {
            FAIL("recv_frame(%s): error %d", messages[i], rxlen);
            failures++;
            continue;
        }

        if ((size_t)rxlen != len || memcmp(rxbuf, payload, len) != 0) {
            FAIL("data mismatch: sent '%s', received '%.*s'", messages[i], rxlen, rxbuf);
            failures++;
        } else {
            PASS("loopback: '%s'", messages[i]);
        }
    }

    return failures;
}

/* ─── Test 2: Byte-stuffing round-trip ──────────────────────────────────── */

static int test_byte_stuffing(void)
{
    printf("\n--- Test 2: Byte-stuffing (0x7E and 0x7D in payload) ---\n");

    /*
     * This payload deliberately contains FRAME_START (0x7E) and FRAME_ESC (0x7D).
     * If byte-stuffing is broken, the receiver will mis-interpret 0x7E as a new
     * frame start mid-payload and corrupt the data.
     */
    static const uint8_t evil_payload[] = {
        0x00, 0x7E, 0x01,       /* 0x7E in the middle */
        0x7D, 0xFF,             /* 0x7D (escape byte itself) */
        0x7E, 0x7E, 0x7D,       /* run of special bytes */
        0x42                    /* normal byte at end */
    };
    uint8_t len = sizeof(evil_payload);
    uint8_t rxbuf[FRAME_MAX_PAYLOAD];
    int     rxlen;

    if (send_frame(tx_fd, evil_payload, len) < 0) {
        FAIL0("send_frame (stuffing test): I/O error");
        return 1;
    }

    rxlen = recv_frame(rx_fd, rxbuf);
    if (rxlen < 0) {
        FAIL("recv_frame (stuffing test): error %d", rxlen);
        return 1;
    }

    if ((size_t)rxlen == len && memcmp(rxbuf, evil_payload, len) == 0) {
        PASS0("byte-stuffing: 0x7E and 0x7D survive the round-trip correctly");
        return 0;
    } else {
        FAIL0("byte-stuffing: data mismatch after round-trip");
        printf("  sent:     ");
        for (uint8_t i = 0; i < len; i++) printf("%02x ", evil_payload[i]);
        printf("\n  received: ");
        for (int i = 0; i < rxlen; i++) printf("%02x ", rxbuf[i]);
        printf("\n");
        return 1;
    }
}

/* ─── Test 3: Corruption detection ─────────────────────────────────────── */

/*
 * Strategy: send a known good frame, then send a corrupted frame (bad checksum
 * injected via a raw write to the fd), then send another good frame.
 * Expected: bad frame is rejected (FRAME_ERR_CHECKSUM), good frame succeeds.
 *
 * We simulate corruption by constructing a raw byte sequence with a flipped
 * checksum byte, then writing it directly, bypassing send_frame().
 */
static int test_corruption_detection(void)
{
    printf("\n--- Test 3: Corruption detection ---\n");

    /* First: a good frame to confirm the channel is working */
    static const uint8_t good_payload[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    uint8_t rxbuf[FRAME_MAX_PAYLOAD];
    int     rxlen, failures = 0;

    if (send_frame(tx_fd, good_payload, sizeof(good_payload)) < 0) {
        FAIL0("send_frame (pre-corruption good frame): I/O error");
        return 1;
    }
    rxlen = recv_frame(rx_fd, rxbuf);
    if (rxlen == (int)sizeof(good_payload) &&
        memcmp(rxbuf, good_payload, sizeof(good_payload)) == 0) {
        INFO0("pre-corruption good frame received correctly");
    } else {
        FAIL0("pre-corruption frame failed \xe2\x80\x94 channel not working");
        return 1;
    }

    /* Corrupted frame: manually build the wire bytes with a wrong checksum */
    /* Good checksum = XOR(0xDE, 0xAD, 0xBE, 0xEF) = 0xDE^0xAD = 0x73,
     *                 0x73^0xBE = 0xCD, 0xCD^0xEF = 0x22
     * We flip bit 0: corrupt_csum = 0x23 */
    static const uint8_t corrupt_frame[] = {
        FRAME_START,    /* 0x7E */
        0x04,           /* LEN = 4 */
        0xDE, 0xAD, 0xBE, 0xEF,  /* payload (no stuffing needed) */
        0x23            /* WRONG checksum (correct is 0x22) */
    };

    INFO0("injecting corrupted frame (checksum 0x23, correct is 0x22)...");
    ssize_t w = write(tx_fd, corrupt_frame, sizeof(corrupt_frame));
    if (w != (ssize_t)sizeof(corrupt_frame)) {
        FAIL0("write corrupted frame: I/O error");
        return 1;
    }

    /* The receiver should detect the bad checksum, log a warning, and resync.
     * Send a recovery frame immediately after. */
    static const uint8_t recovery_payload[] = { 'O', 'K' };
    if (send_frame(tx_fd, recovery_payload, sizeof(recovery_payload)) < 0) {
        FAIL0("send recovery frame: I/O error");
        return 1;
    }

    /*
     * Now receive. The first recv_frame call should:
     *   - Read the corrupt frame
     *   - Detect checksum mismatch
     *   - Resync (seek to next 0x7E)
     *   - Return the recovery frame's data
     *
     * The stderr warning from uart_frame.c is expected here.
     */
    INFO0("calling recv_frame \xe2\x80\x94 expect a checksum warning on stderr:");
    rxlen = recv_frame(rx_fd, rxbuf);
    if (rxlen == (int)sizeof(recovery_payload) &&
        memcmp(rxbuf, recovery_payload, sizeof(recovery_payload)) == 0) {
        PASS0("corruption detected + resync: recovery frame received correctly");
    } else if (rxlen == FRAME_ERR_CHECKSUM) {
        FAIL0("recv_frame returned FRAME_ERR_CHECKSUM \xe2\x80\x94 resync did not recover the next frame");
        failures++;
    } else {
        FAIL("unexpected rxlen=%d after corruption test", rxlen);
        failures++;
    }

    return failures;
}

/* ─── Test 4: Resync after garbage ──────────────────────────────────────── */

static int test_resync_after_garbage(void)
{
    printf("\n--- Test 4: Resync after garbage bytes ---\n");

    /* Write raw garbage that looks nothing like a valid frame */
    static const uint8_t garbage[] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77
    };
    INFO0("injecting 8 garbage bytes...");
    write(tx_fd, garbage, sizeof(garbage));

    /* Now send a valid frame */
    static const uint8_t payload[] = { 'R', 'e', 's', 'y', 'n', 'c' };
    if (send_frame(tx_fd, payload, sizeof(payload)) < 0) {
        FAIL0("send_frame (resync test): I/O error");
        return 1;
    }

    uint8_t rxbuf[FRAME_MAX_PAYLOAD];
    int rxlen = recv_frame(rx_fd, rxbuf);

    if (rxlen == (int)sizeof(payload) &&
        memcmp(rxbuf, payload, sizeof(payload)) == 0) {
        PASS("resync: garbage ignored, valid frame '%.*s' received", rxlen, rxbuf);
        return 0;
    } else {
        FAIL("resync failed: rxlen=%d", rxlen);
        return 1;
    }
}

/* ─── main ──────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *tx_dev, *rx_dev;

    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  %s <device>           -- single device loopback (TX jumpered to RX)\n"
            "  %s <tx_dev> <rx_dev>  -- two linked pty endpoints (socat)\n"
            "\n"
            "Inside the Linux guest (recommended):\n"
            "  # socat pseudoterminal pair (no hardware needed):\n"
            "  socat -d -d pty,raw,echo=0 pty,raw,echo=0\n"
            "  # note the two /dev/pts/N paths printed, then:\n"
            "  sudo %s /dev/pts/2 /dev/pts/3\n"
            "\n"
            "  # QEMU virtual serial (second PL011, /dev/ttyAMA1 in guest):\n"
            "  sudo %s /dev/ttyAMA1\n"
            "\n"
            "On macOS (hardware loopback):\n"
            "  %s /dev/tty.usbserial-0001\n",
            argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    tx_dev = argv[1];
    rx_dev = (argc >= 3) ? argv[2] : argv[1];

    printf("=== UART Framing Protocol Test Suite ===\n");
    printf("TX: %s\n", tx_dev);
    printf("RX: %s\n", rx_dev);
    printf("Baud: 115200, 8N1, raw\n");

    tx_fd = serial_open(tx_dev, B115200);
    if (tx_fd < 0) return 1;

    if (strcmp(tx_dev, rx_dev) == 0) {
        rx_fd = tx_fd;
    } else {
        rx_fd = serial_open(rx_dev, B115200);
        if (rx_fd < 0) { close(tx_fd); return 1; }
    }

    int total_failures = 0;
    total_failures += test_basic_loopback();
    total_failures += test_byte_stuffing();
    total_failures += test_corruption_detection();
    total_failures += test_resync_after_garbage();

    printf("\n=== Results ===\n");
    if (total_failures == 0)
        printf(GREEN "All tests passed.\n" RESET);
    else
        printf(RED "%d test(s) failed.\n" RESET, total_failures);

    if (rx_fd != tx_fd) close(rx_fd);
    close(tx_fd);
    return total_failures ? 1 : 0;
}
