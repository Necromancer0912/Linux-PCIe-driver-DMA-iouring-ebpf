/* SPDX-License-Identifier: MIT */
/*
 * uart_reliable.h — Stop-and-wait reliable transport over byte-stuffed UART
 *
 * Extends uart_frame.h with:
 *   - Frame type byte (DATA / ACK / NACK)
 *   - 1-byte sequence number (wraps mod 256)
 *   - Sender retransmits on NACK or timeout (configurable, default 200 ms)
 *   - Receiver ACKs good frames, ignores duplicate (same SEQ as last)
 *
 * Wire format (all bytes after START are byte-stuffed the same way as
 * uart_frame.c — 0x7E and 0x7D escaped with XOR 0x20):
 *
 *   DATA frame:  [0x7E][TYPE=0x01][SEQ][LEN][PAYLOAD...][CSUM]
 *   ACK  frame:  [0x7E][TYPE=0x02][SEQ][CSUM]
 *   NACK frame:  [0x7E][TYPE=0x03][SEQ][CSUM]
 *
 *   CSUM = XOR over all bytes from TYPE through end of PAYLOAD (inclusive).
 *   An ACK/NACK CSUM covers only TYPE and SEQ.
 *
 * Usage (sender side):
 *   struct reliable_ctx ctx;
 *   reliable_init(&ctx, tx_fd, rx_fd);
 *
 *   int rc = reliable_send(&ctx, payload, len);
 *   // rc >= 0: bytes sent; rc < 0: RELIABLE_ERR_* (see below)
 *
 * Usage (receiver side):
 *   uint8_t buf[RELIABLE_MAX_PAYLOAD];
 *   int rc = reliable_recv(&ctx, buf);
 *   // rc >= 0: payload length; rc < 0: error code
 */

#ifndef UART_RELIABLE_H
#define UART_RELIABLE_H

#include <stdint.h>
#include <stddef.h>

/* ─── Frame type bytes ────────────────────────────────────────────────────── */
#define RFRAME_TYPE_DATA    0x01u   /* Carries payload */
#define RFRAME_TYPE_ACK     0x02u   /* Positive acknowledgement */
#define RFRAME_TYPE_NACK    0x03u   /* Negative acknowledgement (bad checksum) */

/* ─── Protocol limits ────────────────────────────────────────────────────── */
#define RELIABLE_MAX_PAYLOAD    250     /* Fits in 1 LEN byte with overhead */
#define RELIABLE_MAX_RETRIES    3       /* Retransmit attempts before giving up */
#define RELIABLE_TIMEOUT_MS     200     /* ACK wait timeout in milliseconds */

/* ─── Error codes ────────────────────────────────────────────────────────── */
#define RELIABLE_ERR_TIMEOUT    (-1)    /* No ACK within RELIABLE_TIMEOUT_MS */
#define RELIABLE_ERR_NACK       (-2)    /* Receiver sent NACK (bad checksum) */
#define RELIABLE_ERR_MAXRETRY   (-3)    /* Gave up after RELIABLE_MAX_RETRIES */
#define RELIABLE_ERR_IO         (-4)    /* Underlying read/write failed */
#define RELIABLE_ERR_FRAME      (-5)    /* Malformed frame received */
#define RELIABLE_ERR_TOOBIG     (-6)    /* Payload > RELIABLE_MAX_PAYLOAD */

/* ─── Context ────────────────────────────────────────────────────────────── */
struct reliable_ctx {
    int tx_fd;          /* Write end of the serial channel */
    int rx_fd;          /* Read end  of the serial channel */
    uint8_t tx_seq;     /* Next sequence number to send (0..255, wraps) */
    uint8_t rx_seq;     /* Last sequence number successfully received */
    int rx_seq_valid;   /* 0 on first call — accept any sequence number */

    /* Statistics (inspect after a session to see retransmit rate) */
    unsigned long stat_sends;
    unsigned long stat_retransmits;
    unsigned long stat_recvs;
    unsigned long stat_dups;
};

/* ─── API ─────────────────────────────────────────────────────────────────── */

/**
 * reliable_init - initialise context
 * @ctx:    Pointer to caller-allocated context struct
 * @tx_fd:  File descriptor to write frames to
 * @rx_fd:  File descriptor to read ACK/NACK/DATA from
 *          (may equal tx_fd for full-duplex ports)
 */
void reliable_init(struct reliable_ctx *ctx, int tx_fd, int rx_fd);

/**
 * reliable_send - send one payload with stop-and-wait reliability
 * @ctx:        Protocol context
 * @payload:    Data to send (up to RELIABLE_MAX_PAYLOAD bytes)
 * @len:        Payload length
 *
 * Returns 0 on success, or a negative RELIABLE_ERR_* code.
 *
 * Internally: builds and transmits a DATA frame, then waits up to
 * RELIABLE_TIMEOUT_MS for an ACK matching the current sequence number.
 * On NACK or timeout the frame is retransmitted (up to RELIABLE_MAX_RETRIES).
 */
int reliable_send(struct reliable_ctx *ctx,
                  const uint8_t *payload, size_t len);

/**
 * reliable_recv - receive one payload
 * @ctx:    Protocol context
 * @buf:    Caller buffer, at least RELIABLE_MAX_PAYLOAD bytes
 *
 * Returns payload length on success, or a negative RELIABLE_ERR_* code.
 *
 * Internally: waits for a DATA frame, validates checksum, sends ACK on
 * success (or NACK on checksum failure), drops duplicate frames (same SEQ
 * as the last successfully received frame — re-sends ACK without delivering
 * the payload again).
 */
int reliable_recv(struct reliable_ctx *ctx, uint8_t *buf);

/**
 * reliable_stats - print session statistics to stdout
 */
void reliable_stats(const struct reliable_ctx *ctx);

#endif /* UART_RELIABLE_H */
