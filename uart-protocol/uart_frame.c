/*
 * uart_frame.c — Checksummed, byte-stuffed UART framing protocol
 *
 * Implements the API declared in uart_frame.h.
 * Compiles and runs natively on macOS (no VM, no kernel code).
 *
 * Build:
 *   clang -Wall -Wextra -O2 -o uart_test uart_frame.c uart_test.c
 */

#include "uart_frame.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>

/* ─── Checksum ──────────────────────────────────────────────────────────── */

uint8_t frame_checksum(const uint8_t *data, uint8_t len)
{
    uint8_t csum = 0;
    for (uint8_t i = 0; i < len; i++)
        csum ^= data[i];
    return csum;
}

/* ─── Low-level I/O helpers ─────────────────────────────────────────────── */

/*
 * read_byte() — read exactly one byte, retrying on EINTR.
 * Returns 1 on success, 0 on EOF, -1 on error.
 */
static int read_byte(int fd, uint8_t *b)
{
    ssize_t n;
    do {
        n = read(fd, b, 1);
    } while (n < 0 && errno == EINTR);

    if (n == 1) return 1;
    if (n == 0) return 0;   /* EOF / device closed */
    return -1;
}

/*
 * write_byte() — write exactly one byte.
 * Returns 1 on success, -1 on error.
 */
static int write_byte(int fd, uint8_t b)
{
    ssize_t n;
    do {
        n = write(fd, &b, 1);
    } while (n < 0 && errno == EINTR);
    return (n == 1) ? 1 : -1;
}

/* ─── Byte stuffing ─────────────────────────────────────────────────────── */

/*
 * stuff_byte() — write one byte with stuffing applied.
 * If the byte is 0x7E or 0x7D, emit the two-byte escape sequence instead.
 * Returns bytes written (1 or 2), or -1 on I/O error.
 */
static int stuff_byte(int fd, uint8_t b)
{
    if (b == FRAME_START || b == FRAME_ESC) {
        if (write_byte(fd, FRAME_ESC) < 0) return -1;
        if (write_byte(fd, b ^ FRAME_ESC_XOR) < 0) return -1;
        return 2;
    }
    if (write_byte(fd, b) < 0) return -1;
    return 1;
}

/* ─── send_frame ────────────────────────────────────────────────────────── */

int send_frame(int fd, const uint8_t *payload, uint8_t len)
{
    uint8_t csum;
    int     written = 0;
    int     rc;

    /* START delimiter — never stuffed (it is the frame marker) */
    if (write_byte(fd, FRAME_START) < 0) return -1;
    written++;

    /* LEN — stuffed in case len happens to be 0x7E or 0x7D */
    rc = stuff_byte(fd, len);
    if (rc < 0) return -1;
    written += rc;

    /* PAYLOAD bytes — each stuffed individually */
    csum = 0;
    for (uint8_t i = 0; i < len; i++) {
        csum ^= payload[i];
        rc = stuff_byte(fd, payload[i]);
        if (rc < 0) return -1;
        written += rc;
    }

    /* CHECKSUM — stuffed */
    rc = stuff_byte(fd, csum);
    if (rc < 0) return -1;
    written += rc;

    return written;
}

/* ─── recv_frame ────────────────────────────────────────────────────────── */

int recv_frame(int fd, uint8_t *buf)
{
    uint8_t b, len, rx_csum, computed_csum;
    int     rc;

resync:
    /* ── Phase 1: scan for START byte ─────────────────────────────────── */
    do {
        rc = read_byte(fd, &b);
        if (rc < 0) return FRAME_ERR_IO;
        if (rc == 0) return FRAME_ERR_IO;   /* EOF */
    } while (b != FRAME_START);

    /* ── Phase 2: read LEN (with un-stuffing) ─────────────────────────── */
    rc = read_byte(fd, &b);
    if (rc <= 0) return FRAME_ERR_IO;

    if (b == FRAME_ESC) {
        /* Escape: next byte is the actual LEN XOR ESC_XOR */
        rc = read_byte(fd, &b);
        if (rc <= 0) return FRAME_ERR_IO;
        b ^= FRAME_ESC_XOR;
    } else if (b == FRAME_START) {
        /* Unexpected START — previous frame was truncated, resync */
        goto resync;
    }
    len = b;

    if (len > FRAME_MAX_PAYLOAD)
        return FRAME_ERR_OVERFLOW;

    /* ── Phase 3: read PAYLOAD bytes (with un-stuffing) ──────────────── */
    computed_csum = 0;
    for (uint8_t i = 0; i < len; i++) {
        rc = read_byte(fd, &b);
        if (rc <= 0) return FRAME_ERR_IO;

        if (b == FRAME_ESC) {
            rc = read_byte(fd, &b);
            if (rc <= 0) return FRAME_ERR_IO;
            b ^= FRAME_ESC_XOR;
        } else if (b == FRAME_START) {
            /* Framing error — truncated frame, resync */
            fprintf(stderr, "[uart_frame] framing error: unexpected START in payload, resyncing\n");
            goto resync;
        }

        buf[i]        = b;
        computed_csum ^= b;
    }

    /* ── Phase 4: read CHECKSUM (with un-stuffing) ────────────────────── */
    rc = read_byte(fd, &b);
    if (rc <= 0) return FRAME_ERR_IO;

    if (b == FRAME_ESC) {
        rc = read_byte(fd, &b);
        if (rc <= 0) return FRAME_ERR_IO;
        b ^= FRAME_ESC_XOR;
    } else if (b == FRAME_START) {
        fprintf(stderr, "[uart_frame] framing error: unexpected START where checksum expected, resyncing\n");
        goto resync;
    }
    rx_csum = b;

    /* ── Phase 5: validate checksum ───────────────────────────────────── */
    if (rx_csum != computed_csum) {
        fprintf(stderr,
                "[uart_frame] checksum MISMATCH: received 0x%02x, computed 0x%02x — frame rejected, resyncing\n",
                rx_csum, computed_csum);
        goto resync;
    }

    return (int)len;
}

/* ─── serial_open ───────────────────────────────────────────────────────── */

int serial_open(const char *device, int baud)
{
    int fd;
    struct termios tty;

    fd = open(device, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
        perror("serial_open: open");
        return -1;
    }

    if (tcgetattr(fd, &tty) < 0) {
        perror("serial_open: tcgetattr");
        close(fd);
        return -1;
    }

    /* Raw mode: disable all input/output processing */
    cfmakeraw(&tty);

    /* Baud rate */
    cfsetispeed(&tty, baud);
    cfsetospeed(&tty, baud);

    /* 8N1: 8 data bits, no parity, 1 stop bit */
    tty.c_cflag &= ~PARENB;         /* no parity */
    tty.c_cflag &= ~CSTOPB;         /* 1 stop bit */
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;             /* 8 data bits */

    /* No hardware flow control */
    tty.c_cflag &= ~CRTSCTS;

    /* Enable receiver, ignore modem status lines */
    tty.c_cflag |= CREAD | CLOCAL;

    /*
     * Read timeout: VTIME = 10 (1 second), VMIN = 0
     * read() returns after 1 second even if no bytes arrived.
     * This prevents recv_frame() from blocking forever on a dead link.
     */
    tty.c_cc[VTIME] = 10;
    tty.c_cc[VMIN]  = 0;

    if (tcsetattr(fd, TCSANOW, &tty) < 0) {
        perror("serial_open: tcsetattr");
        close(fd);
        return -1;
    }

    /* Flush any stale bytes in the driver buffer */
    tcflush(fd, TCIOFLUSH);

    return fd;
}
