/*
 * uart_frame.h — Checksummed, byte-stuffed UART framing protocol
 *
 * Frame format (on the wire, after byte-stuffing):
 *
 *   [START=0x7E] [LEN] [PAYLOAD...] [CHECKSUM]
 *
 * Where:
 *   START    — frame delimiter, always 0x7E
 *   LEN      — number of payload bytes BEFORE stuffing (0–FRAME_MAX_PAYLOAD)
 *   PAYLOAD  — the actual data bytes (byte-stuffed, see below)
 *   CHECKSUM — XOR of all unstuffed payload bytes
 *
 * Byte stuffing (similar to PPP/HDLC):
 *   If any byte in LEN, PAYLOAD, or CHECKSUM equals 0x7E or 0x7D,
 *   it is replaced by the two-byte sequence:
 *     [FRAME_ESC=0x7D] [byte XOR FRAME_ESC_XOR=0x20]
 *   The receiver reverses this on the fly.
 *
 * Resync:
 *   The receiver discards all bytes until it sees 0x7E, then expects LEN
 *   next. If a checksum fails, it discards bytes and resyncs on the next 0x7E.
 *   This makes the protocol robust against bit errors and mid-stream start.
 *
 * Runs natively on macOS — no VM required.
 * Tested via loopback (TX pin jumpered to RX) or socat pseudoterminals.
 */

#ifndef UART_FRAME_H
#define UART_FRAME_H

#include <stdint.h>
#include <stddef.h>

/* ── Frame constants ─────────────────────────────────────────────────────── */
#define FRAME_START         0x7Eu   /* Start-of-frame delimiter              */
#define FRAME_ESC           0x7Du   /* Escape byte (stuffing)                */
#define FRAME_ESC_XOR       0x20u   /* XOR mask applied to stuffed byte      */
#define FRAME_MAX_PAYLOAD   255u    /* Maximum payload bytes before stuffing */

/* ── Error codes returned by recv_frame ─────────────────────────────────── */
#define FRAME_ERR_CHECKSUM  (-1)    /* Checksum mismatch — frame rejected     */
#define FRAME_ERR_IO        (-2)    /* I/O error or connection closed         */
#define FRAME_ERR_TIMEOUT   (-3)    /* Read timeout expired                   */
#define FRAME_ERR_OVERFLOW  (-4)    /* Payload larger than FRAME_MAX_PAYLOAD  */

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * frame_checksum() — compute XOR checksum over payload bytes
 * @data: pointer to payload
 * @len:  number of bytes
 *
 * Returns XOR of all bytes. Chosen for simplicity; can be upgraded to
 * CRC-8 or CRC-16 without changing the frame format.
 */
uint8_t frame_checksum(const uint8_t *data, uint8_t len);

/**
 * send_frame() — encode and transmit one frame
 * @fd:      open serial file descriptor (O_RDWR)
 * @payload: raw payload bytes (before byte-stuffing)
 * @len:     payload length (max FRAME_MAX_PAYLOAD)
 *
 * Applies byte-stuffing, prepends START and LEN, appends CHECKSUM,
 * then writes the whole frame in one call.
 *
 * Returns total bytes written on success, -1 on error (errno set).
 */
int send_frame(int fd, const uint8_t *payload, uint8_t len);

/**
 * recv_frame() — receive and decode one frame
 * @fd:  open serial file descriptor (O_RDWR)
 * @buf: buffer to receive decoded payload (must be >= FRAME_MAX_PAYLOAD bytes)
 *
 * Reads byte-by-byte. Discards bytes until 0x7E is seen (resync). Then reads
 * LEN, reads that many payload bytes (un-stuffing inline), validates checksum.
 * On checksum mismatch: logs a warning and resyncs for the next frame.
 *
 * Returns: payload length (>= 0) on success
 *          FRAME_ERR_* negative code on error
 */
int recv_frame(int fd, uint8_t *buf);

/**
 * serial_open() — open and configure a serial port
 * @device: path to the tty device (e.g. "/dev/tty.usbserial-0001")
 * @baud:   baud rate constant (e.g. B115200) from <termios.h>
 *
 * Configures: raw mode (cfmakeraw), 8N1, no flow control, read timeout 1s.
 *
 * Returns: file descriptor on success, -1 on error (errno set).
 */
int serial_open(const char *device, int baud);

#endif /* UART_FRAME_H */
