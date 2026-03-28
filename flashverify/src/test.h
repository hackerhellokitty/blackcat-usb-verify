#ifndef TEST_H
#define TEST_H

#include <stdint.h>
#include <stddef.h>
#include "rawio.h"

typedef enum {
    CHUNK_PENDING    = 0,
    CHUNK_WRITTEN    = 1,
    CHUNK_OK         = 2,
    CHUNK_MISMATCH   = 3,
    CHUNK_UNREADABLE = 4,
} VerifyStatus;

typedef struct {
    uint64_t     offset;
    uint64_t     length;
    uint8_t      sha256[32];
    VerifyStatus status;
    double       write_ms;
    double       read_ms;
} ChunkRecord;

typedef enum {
    STATE_IDLE      = 0,
    STATE_WRITING   = 1,
    STATE_VERIFYING = 2,
    STATE_DONE      = 3,
    STATE_CANCELLED = 4,
    STATE_ERROR     = 5,
} TestState;

typedef enum {
    VERDICT_GENUINE     = 0,
    VERDICT_COUNTERFEIT = 1,
    VERDICT_WARNING     = 2,
} Verdict;

typedef struct TestSession {
    /* device info */
    char     device_path[512];
    char     device_name[256];
    char     device_serial[64];
    char     vendor_id[32];
    char     product_id[32];

    /* capacity */
    uint64_t claimed_bytes;
    uint64_t actual_bytes;
    uint64_t chunk_size;

    /* test data */
    ChunkRecord *chunks;
    size_t       chunk_count;
    size_t       ok_count;
    size_t       fail_count;

    /* state */
    TestState state;
    int       cancel_flag;
    char      error_msg[256];

    /* performance */
    double avg_write_mbps;
    double avg_read_mbps;

    /* order info */
    char order_id[64];
    char shop_name[128];
    char platform[32];
    char purchase_price[32];

    /* progress callback (worker → UI) */
    void (*on_progress)(struct TestSession *s, void *user_data);
    void *callback_data;
} TestSession;

/* lifecycle */
int         session_init(TestSession *s);
void        session_free(TestSession *s);

/* passes — take RawDevice (HANDLE on Win, fd on Linux) */
int         write_pass(TestSession *s, RawDevice dev);
int         verify_pass(TestSession *s, RawDevice dev);

/* verdict */
Verdict     verdict(TestSession *s);
const char *verdict_str(Verdict v);

#endif /* TEST_H */
