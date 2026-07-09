/* SPDX-License-Identifier: MIT */
/*
 * uart_reliable.c — Stop-and-wait reliable transport over raw UART
 *
 * Layered on top of uart_frame.c byte-stuffed framing. This layer adds:
 *   - Frame type byte so sender and receiver agree on DATA vs control frames
 *   - 1-byte sequence number wrapping mod 256
 *   - Receiver ACKs good frames, NACKs bad-checksum frames
 *   - Sender retransmits on NACK or timeout, gives up after MAX_RETRIES
 *   - Duplicate DATA frames (same SEQ) are silently re-ACK'd, not delivered
 *
 * Design: stop-and-wait (Go-Back-1 window = 1). Enough for low-bandwidth
 * embedded links; the architecture scales to sliding-window by extending
 * tx_seq/rx_seq to multi-bit windows (as in TCP or HDLC extended mode).
 */

#include "uart_reliable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <stdint.h>

/* ─── Frame constants ─────────────────────────────────────────────────────── */
#define START_BYTE  0x7Eu
#define ESC_BYTE    0x7Du
#define XOR_VAL     0x20u

/* ─── Internal frame sizes ───────────────────────────────────────────────────
 * DATA:  1 type + 1 seq + 1 len + up to 250 payload + 1 csum = 254 max
 * ACK/NACK: 1 type + 1 seq + 1 csum = 3
 */
#define DATA_HDR_LEN    3   /* type, seq, len */
#define ACK_LEN         3   /* type, seq, csum */
#define RAW_MAX         (DATA_HDR_LEN + RELIABLE_MAX_PAYLOAD + 1 /* csum */)

/* ─── Helpers ─────────────────────────────────────────────────────────────── */

/* Compute XOR checksum over a byte array */
static uint8_t csum(const uint8_t *data, size_t len)
{
    uint8_t c = 0;
    while (len--)
        c ^= *data++;
    return c;
}

/*
 * stuff_and_write — byte-stuff src[] and write to fd.
 *
 * Output format:  [START_BYTE][stuffed bytes...]
 * Stuffing rule: 0x7E → 0x7D 0x5E,  0x7D → 0x7D 0x5D
 *   (same as uart_frame.c so the two layers are wire-compatible in framing)
 */
static int stuff_and_write(int fd, const uint8_t *src, size_t len)
{
    /* Worst case: every byte needs escaping */
    uint8_t buf[1 + RAW_MAX * 2];
    size_t  out = 0;

    buf[out++] = START_BYTE;

    for (size_t i = 0; i < len; i++) {
        if (src[i] == START_BYTE || src[i] == ESC_BYTE) {
            buf[out++] = ESC_BYTE;
            buf[out++] = src[i] ^ XOR_VAL;
        } else {
            buf[out++] = src[i];
        }
    }

    ssize_t w = write(fd, buf, out);
    return (w == (ssize_t)out) ? 0 : -1;
}

/*
 * timed_read_byte — read one byte from fd with a millisecond timeout.
 * Returns the byte (0–255) or -1 on timeout / error.
 */
static int timed_read_byte(int fd, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int rc = poll(&pfd, 1, timeout_ms);
    if (rc <= 0) return -1;     /* timeout or error */

    uint8_t b;
    if (read(fd, &b, 1) != 1)
        return -1;
    return b;
}

/*
 * recv_unstuffed — read one logical frame from rx_fd (timeout_ms per byte).
 *
 * Scans for START_BYTE, then reads de-stuffed bytes into buf[].
 * A second START_BYTE ends the current frame (the protocol does not use
 * an explicit end byte; a new START_BYTE restarts framing).
 *
 * Returns number of de-stuffed bytes in buf, or -1 on timeout/error.
 *
 * Maximum wait is roughly timeout_ms * (frame_len) which is fine for
 * small frames at normal baud rates.
 */
static int recv_unstuffed(int rx_fd, uint8_t *buf, size_t bufsz, int timeout_ms)
{
    /* Scan for START_BYTE */
    for (;;) {
        int b = timed_read_byte(rx_fd, timeout_ms);
        if (b < 0)
            return -1;
        if ((uint8_t)b == START_BYTE)
            break;
    }

    size_t n = 0;
    int    in_esc = 0;

    for (;;) {
        int b = timed_read_byte(rx_fd, timeout_ms);
        if (b < 0)
            return -1;   /* timeout mid-frame */

        if ((uint8_t)b == START_BYTE)
            break;   /* new frame starts — end of current */

        if (in_esc) {
            if (n >= bufsz) return -1;
            buf[n++] = (uint8_t)(b ^ XOR_VAL);
            in_esc = 0;
        } else if ((uint8_t)b == ESC_BYTE) {
            in_esc = 1;
        } else {
            if (n >= bufsz) return -1;
            buf[n++] = (uint8_t)b;
        }
    }

    return (int)n;
}

/* ─── Public API ──────────────────────────────────────────────────────────── */

void reliable_init(struct reliable_ctx *ctx, int tx_fd, int rx_fd)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->tx_fd        = tx_fd;
    ctx->rx_fd        = rx_fd;
    ctx->tx_seq       = 0;
    ctx->rx_seq       = 0;
    ctx->rx_seq_valid = 0;
}

/*
 * send_data_frame — build and transmit one DATA frame.
 *
 * Frame body (before stuffing):
 *   [TYPE=0x01][SEQ][LEN][payload...][CSUM]
 * where CSUM = XOR(TYPE, SEQ, LEN, payload...).
 */
static int send_data_frame(struct reliable_ctx *ctx,
                           const uint8_t *payload, size_t len)
{
    uint8_t body[RAW_MAX];
    size_t  pos = 0;

    body[pos++] = RFRAME_TYPE_DATA;
    body[pos++] = ctx->tx_seq;
    body[pos++] = (uint8_t)len;
    memcpy(&body[pos], payload, len);
    pos += len;
    /* Compute CSUM before advancing pos to avoid unsequenced-modification UB */
    uint8_t cs = csum(body, pos);  /* covers type+seq+len+payload */
    body[pos++] = cs;

    return stuff_and_write(ctx->tx_fd, body, pos);
}

/*
 * send_ack / send_nack — build and transmit a 3-byte control frame.
 *
 * Frame body (before stuffing):
 *   [TYPE][SEQ][CSUM]  where CSUM = XOR(TYPE, SEQ)
 */
static int send_control(int fd, uint8_t type, uint8_t seq)
{
    uint8_t body[3] = { type, seq, type ^ seq };
    return stuff_and_write(fd, body, 3);
}

int reliable_send(struct reliable_ctx *ctx,
                  const uint8_t *payload, size_t len)
{
    if (len > RELIABLE_MAX_PAYLOAD)
        return RELIABLE_ERR_TOOBIG;

    ctx->stat_sends++;

    for (int attempt = 0; attempt <= RELIABLE_MAX_RETRIES; attempt++) {
        if (attempt > 0) {
            ctx->stat_retransmits++;
            fprintf(stderr, "[reliable] retransmit attempt %d (seq=%u)\n",
                    attempt, ctx->tx_seq);
        }

        /* Transmit DATA frame */
        if (send_data_frame(ctx, payload, len) < 0)
            return RELIABLE_ERR_IO;

        /*
         * Wait for ACK or NACK.
         * We read an entire control frame (3 de-stuffed bytes: type, seq, csum).
         * If the response type or checksum is wrong, treat as timeout and retry.
         */
        uint8_t resp[ACK_LEN + 4]; /* a little slack */
        int rlen = recv_unstuffed(ctx->rx_fd, resp, sizeof(resp),
                                  RELIABLE_TIMEOUT_MS);

        if (rlen < 0) {
            /* Timeout */
            fprintf(stderr, "[reliable] timeout waiting for ACK (seq=%u)\n",
                    ctx->tx_seq);
            continue;
        }

        if (rlen < 3)
            continue;   /* too short to be a valid control frame */

        uint8_t rtype = resp[0];
        uint8_t rseq  = resp[1];
        uint8_t rcsum = resp[2];

        /* Validate control frame checksum */
        if (rcsum != (rtype ^ rseq))
            continue;

        /* Only accept response for our current sequence number */
        if (rseq != ctx->tx_seq)
            continue;

        if (rtype == RFRAME_TYPE_ACK) {
            ctx->tx_seq++;   /* advance sequence number */
            return 0;
        }

        if (rtype == RFRAME_TYPE_NACK) {
            fprintf(stderr, "[reliable] NACK received (seq=%u) — retransmitting\n",
                    ctx->tx_seq);
            continue;
        }
        /* Unknown type — treat as garbled, retry */
    }

    return RELIABLE_ERR_MAXRETRY;
}

int reliable_recv(struct reliable_ctx *ctx, uint8_t *buf)
{
    for (;;) {
        /*
         * Read one unstuffed frame. Use a generous timeout here since the
         * receiver is passive — it can wait as long as needed for the sender.
         * Use 5 seconds per byte; callers that need a finite wait should wrap
         * this in a thread or use select/poll on the fd themselves.
         */
        uint8_t raw[RAW_MAX + 4];
        int rlen = recv_unstuffed(ctx->rx_fd, raw, sizeof(raw), 5000);

        if (rlen < 0)
            return RELIABLE_ERR_IO;

        /* Minimum DATA frame: type(1) + seq(1) + len(1) + csum(1) = 4 */
        if (rlen < 4)
            continue;

        uint8_t rtype = raw[0];
        if (rtype != RFRAME_TYPE_DATA)
            continue;   /* ignore stray ACK/NACK (from previous session) */

        uint8_t rseq     = raw[1];
        uint8_t plen_raw = raw[2];
        int     expected_total = 3 + (int)plen_raw + 1; /* type+seq+len + payload + csum */

        if (rlen < expected_total)
            continue;   /* truncated frame */

        /* Validate checksum over type + seq + len + payload */
        uint8_t expected_csum = csum(raw, (size_t)(3 + plen_raw));
        uint8_t actual_csum   = raw[3 + plen_raw];

        if (actual_csum != expected_csum) {
            fprintf(stderr,
                    "[reliable] checksum error: expected 0x%02x got 0x%02x — sending NACK\n",
                    expected_csum, actual_csum);
            send_control(ctx->tx_fd, RFRAME_TYPE_NACK, rseq);
            continue;
        }

        /*
         * Duplicate detection: if we've received this SEQ before, re-ACK it
         * (the ACK was lost, so the sender retransmitted) but don't deliver.
         */
        if (ctx->rx_seq_valid && rseq == ctx->rx_seq) {
            ctx->stat_dups++;
            fprintf(stderr, "[reliable] duplicate frame seq=%u — re-ACKing\n", rseq);
            send_control(ctx->tx_fd, RFRAME_TYPE_ACK, rseq);
            continue;
        }

        /* Good new frame — deliver */
        memcpy(buf, &raw[3], plen_raw);
        send_control(ctx->tx_fd, RFRAME_TYPE_ACK, rseq);

        ctx->rx_seq       = rseq;
        ctx->rx_seq_valid = 1;
        ctx->stat_recvs++;

        return (int)plen_raw;
    }
}

void reliable_stats(const struct reliable_ctx *ctx)
{
    printf("\n─── Reliable UART session statistics ───────────────\n");
    printf("  Frames sent:         %lu\n",  ctx->stat_sends);
    printf("  Retransmits:         %lu  (%.0f%% loss rate)\n",
           ctx->stat_retransmits,
           ctx->stat_sends > 0
               ? 100.0 * (double)ctx->stat_retransmits / (double)ctx->stat_sends
               : 0.0);
    printf("  Frames received:     %lu\n",  ctx->stat_recvs);
    printf("  Duplicates dropped:  %lu\n",  ctx->stat_dups);
    printf("────────────────────────────────────────────────────\n");
}
