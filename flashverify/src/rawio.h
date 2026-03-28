#ifndef RAWIO_H
#define RAWIO_H

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#include <windows.h>

/* On Windows we keep volume handles open to prevent Windows from
 * remounting partitions while we are writing the physical drive.   */
#define RAWIO_MAX_VOL 26

typedef struct {
    HANDLE disk;                    /* \\.\PhysicalDriveN handle  */
    HANDLE vol[RAWIO_MAX_VOL];      /* locked volume handles       */
    int    vol_count;
    int    lock_failed;             /* 1 = at least one volume could not be locked */
} RawDevice;

static inline int rawio_valid(RawDevice d) {
    return d.disk != INVALID_HANDLE_VALUE;
}

extern RawDevice RAW_INVALID_VAL;
#define RAW_INVALID (RAW_INVALID_VAL)

#else
typedef int RawDevice;
#define RAW_INVALID  (-1)
static inline int rawio_valid(RawDevice d) { return d >= 0; }
#endif

/* open / close */
RawDevice rawio_open_rw(const char *path);
RawDevice rawio_open_ro(const char *path);
void      rawio_close(RawDevice dev);

/* aligned read/write at byte offset */
typedef long long rawio_ssize_t;
rawio_ssize_t rawio_pread (RawDevice dev, void       *buf, size_t n, uint64_t offset);
rawio_ssize_t rawio_pwrite(RawDevice dev, const void *buf, size_t n, uint64_t offset);

/* aligned buffer allocation (512-byte aligned) */
void *rawio_alloc(size_t size);
void  rawio_free(void *ptr);

/* timing */
double rawio_now_ms(void);

#define RAWIO_SECTOR 512

#endif /* RAWIO_H */
