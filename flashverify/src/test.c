#include "test.h"
#include "hash.h"
#include "random.h"
#include "rawio.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define CHUNK_DEFAULT (64ULL * 1024 * 1024)   /* 64 MB */

int session_init(TestSession *s) {
    if (s->chunk_size == 0)
        s->chunk_size = CHUNK_DEFAULT;

    /* align chunk_size to sector boundary */
    s->chunk_size = (s->chunk_size / RAWIO_SECTOR) * RAWIO_SECTOR;

    s->chunk_count = (s->actual_bytes + s->chunk_size - 1) / s->chunk_size;
    s->chunks = calloc(s->chunk_count, sizeof(ChunkRecord));
    if (!s->chunks) return -1;

    for (size_t i = 0; i < s->chunk_count; i++) {
        s->chunks[i].offset = i * s->chunk_size;
        s->chunks[i].length = (i + 1 < s->chunk_count)
                              ? s->chunk_size
                              : (s->actual_bytes - i * s->chunk_size);
        s->chunks[i].status = CHUNK_PENDING;
    }

    s->ok_count    = 0;
    s->fail_count  = 0;
    s->state       = STATE_IDLE;
    s->cancel_flag = 0;
    return 0;
}

void session_free(TestSession *s) {
    free(s->chunks);
    s->chunks = NULL;
}

/* ── Write Pass ─────────────────────────────────────────────────────────── */

int write_pass(TestSession *s, RawDevice dev) {
    s->state = STATE_WRITING;

    uint8_t *buf = rawio_alloc(s->chunk_size);
    if (!buf) {
        snprintf(s->error_msg, sizeof(s->error_msg), "OOM: buffer alloc failed");
        s->state = STATE_ERROR;
        return -1;
    }

    double total_bytes = 0.0;
    double total_ms    = 0.0;

    for (size_t i = 0; i < s->chunk_count; i++) {
        if (s->cancel_flag) {
            s->state = STATE_CANCELLED;
            rawio_free(buf);
            return -1;
        }

        ChunkRecord *cr = &s->chunks[i];
        size_t len = (size_t)cr->length;

        /* pad last chunk to sector alignment */
        size_t write_len = (len + RAWIO_SECTOR - 1) & ~(size_t)(RAWIO_SECTOR - 1);

        fill_random_buffer(buf, len);
        if (write_len > len)
            memset(buf + len, 0, write_len - len);

        sha256_chunk(buf, len, cr->sha256);

        double t0 = rawio_now_ms();
        rawio_ssize_t written = rawio_pwrite(dev, buf, write_len, cr->offset);
        double dt = rawio_now_ms() - t0;

        if (written < (rawio_ssize_t)write_len) {
            /* retry once */
            written = rawio_pwrite(dev, buf, write_len, cr->offset);
            if (written < (rawio_ssize_t)write_len) {
                cr->status = CHUNK_UNREADABLE;
                s->fail_count++;
                if (s->on_progress) s->on_progress(s, s->callback_data);
                continue;
            }
        }

        cr->status   = CHUNK_WRITTEN;
        cr->write_ms = dt;
        total_bytes += (double)len;
        total_ms    += dt;

        /* update running average so UI shows speed in realtime */
        if (total_ms > 0.0)
            s->avg_write_mbps = (total_bytes / (1024.0 * 1024.0)) / (total_ms / 1000.0);

        if (s->on_progress) s->on_progress(s, s->callback_data);
    }

    rawio_free(buf);
    return 0;
}

/* ── Verify Pass ────────────────────────────────────────────────────────── */

int verify_pass(TestSession *s, RawDevice dev) {
    s->state = STATE_VERIFYING;

    uint8_t *buf = rawio_alloc(s->chunk_size);
    if (!buf) {
        snprintf(s->error_msg, sizeof(s->error_msg), "OOM: buffer alloc failed");
        s->state = STATE_ERROR;
        return -1;
    }

    uint8_t read_hash[32];
    double total_bytes = 0.0;
    double total_ms    = 0.0;

    for (size_t i = 0; i < s->chunk_count; i++) {
        if (s->cancel_flag) {
            s->state = STATE_CANCELLED;
            rawio_free(buf);
            return -1;
        }

        ChunkRecord *cr = &s->chunks[i];

        if (cr->status == CHUNK_UNREADABLE) {
            if (s->on_progress) s->on_progress(s, s->callback_data);
            continue;
        }

        size_t len      = (size_t)cr->length;
        size_t read_len = (len + RAWIO_SECTOR - 1) & ~(size_t)(RAWIO_SECTOR - 1);

        double t0 = rawio_now_ms();
        rawio_ssize_t got = rawio_pread(dev, buf, read_len, cr->offset);
        double dt = rawio_now_ms() - t0;

        if (got < (rawio_ssize_t)len) {
            cr->status = CHUNK_UNREADABLE;
            s->fail_count++;
            if (s->on_progress) s->on_progress(s, s->callback_data);
            continue;
        }

        sha256_chunk(buf, len, read_hash);
        cr->read_ms  = dt;
        total_bytes += (double)len;
        total_ms    += dt;

        /* update running average */
        if (total_ms > 0.0)
            s->avg_read_mbps = (total_bytes / (1024.0 * 1024.0)) / (total_ms / 1000.0);

        if (memcmp(read_hash, cr->sha256, 32) == 0) {
            cr->status = CHUNK_OK;
            s->ok_count++;
        } else {
            cr->status = CHUNK_MISMATCH;
            s->fail_count++;

            /* loopback detection */
            for (size_t j = 0; j < i; j++) {
                if (memcmp(read_hash, s->chunks[j].sha256, 32) == 0) {
                    fprintf(stderr,
                            "LOOPBACK DETECTED: chunk %zu reads same data as chunk %zu\n",
                            i, j);
                    s->chunks[j].status = CHUNK_MISMATCH;
                    break;
                }
            }
        }

        if (s->on_progress) s->on_progress(s, s->callback_data);
    }

    rawio_free(buf);
    return 0;
}

/* ── Verdict ────────────────────────────────────────────────────────────── */

Verdict verdict(TestSession *s) {
    if (s->actual_bytes < s->claimed_bytes)
        return VERDICT_COUNTERFEIT;

    double fail_ratio = (s->chunk_count > 0)
                        ? (double)s->fail_count / (double)s->chunk_count
                        : 0.0;

    if (fail_ratio == 0.0) return VERDICT_GENUINE;
    if (fail_ratio < 0.05) return VERDICT_WARNING;
    return VERDICT_COUNTERFEIT;
}

const char *verdict_str(Verdict v) {
    switch (v) {
        case VERDICT_GENUINE:     return "GENUINE";
        case VERDICT_COUNTERFEIT: return "COUNTERFEIT";
        case VERDICT_WARNING:     return "WARNING";
    }
    return "UNKNOWN";
}
