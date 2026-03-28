#include "rawio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ════════════════════════════════════════════════════════
 *  WINDOWS
 * ════════════════════════════════════════════════════════ */
#ifdef _WIN32
#include <winioctl.h>

/* sentinel value for RAW_INVALID */
RawDevice RAW_INVALID_VAL = { INVALID_HANDLE_VALUE, {0}, 0 };

static int parse_drive_number(const char *path) {
    const char *p = path;
    while (*p == '\\' || *p == '.') p++;
    for (; *p; p++) {
        if ((_strnicmp(p, "PhysicalDrive", 13) == 0)) {
            return atoi(p + 13);
        }
    }
    return -1;
}

/* Lock + dismount all volumes on the given physical drive number.
 * Keeps volume handles OPEN so Windows cannot remount them.
 * Caller must close them via rawio_close().                        */
static int lock_volumes(int drive_number, HANDLE vol_out[], int max_vol) {
    int n = 0;
    for (char c = 'A'; c <= 'Z' && n < max_vol; c++) {
        char path[8];
        snprintf(path, sizeof(path), "\\\\.\\%c:", c);

        HANDLE hv = CreateFileA(path,
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, 0, NULL);
        if (hv == INVALID_HANDLE_VALUE) continue;

        STORAGE_DEVICE_NUMBER sdn;
        DWORD ret;
        BOOL ok = DeviceIoControl(hv, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                                  NULL, 0, &sdn, sizeof(sdn), &ret, NULL);
        if (!ok || (int)sdn.DeviceNumber != drive_number) {
            CloseHandle(hv);
            continue;
        }

        /* Lock volume — fails if another process has files open */
        BOOL locked = DeviceIoControl(hv, FSCTL_LOCK_VOLUME,
                                      NULL, 0, NULL, 0, &ret, NULL);
        if (!locked) {
            fprintf(stderr, "[rawio] WARNING: cannot lock %s (error %lu) "
                    "— close File Explorer and retry\n",
                    path, GetLastError());
            CloseHandle(hv);
            continue;
        }
        /* Dismount — flushes and detaches filesystem */
        DeviceIoControl(hv, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &ret, NULL);

        /* Keep handle open — closing it lets Windows remount */
        vol_out[n++] = hv;
        fprintf(stderr, "[rawio] locked volume %s OK\n", path);
    }
    return n;
}

RawDevice rawio_open_rw(const char *path) {
    RawDevice d;
    d.disk        = INVALID_HANDLE_VALUE;
    d.vol_count   = 0;
    d.lock_failed = 0;
    memset(d.vol, 0, sizeof(d.vol));

    int drv_num = parse_drive_number(path);
    if (drv_num >= 0) {
        /* count how many volumes exist on this drive */
        int total_vols = 0;
        for (char c = 'A'; c <= 'Z'; c++) {
            char vp[8];
            snprintf(vp, sizeof(vp), "\\\\.\\%c:", c);
            HANDLE hv = CreateFileA(vp, 0,
                FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (hv == INVALID_HANDLE_VALUE) continue;
            STORAGE_DEVICE_NUMBER sdn; DWORD r;
            if (DeviceIoControl(hv, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                    NULL, 0, &sdn, sizeof(sdn), &r, NULL) &&
                (int)sdn.DeviceNumber == drv_num) total_vols++;
            CloseHandle(hv);
        }
        d.vol_count = lock_volumes(drv_num, d.vol, RAWIO_MAX_VOL);
        if (d.vol_count < total_vols)
            d.lock_failed = 1;
    }

    d.disk = CreateFileA(
        path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
        NULL);

    if (d.disk == INVALID_HANDLE_VALUE) {
        for (int i = 0; i < d.vol_count; i++) CloseHandle(d.vol[i]);
        d.vol_count   = 0;
        d.lock_failed = 0;
    }
    return d;
}

RawDevice rawio_open_ro(const char *path) {
    RawDevice d;
    d.disk        = INVALID_HANDLE_VALUE;
    d.vol_count   = 0;
    d.lock_failed = 0;
    memset(d.vol, 0, sizeof(d.vol));

    d.disk = CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING,
        NULL);
    return d;
}

void rawio_close(RawDevice dev) {
    if (dev.disk != INVALID_HANDLE_VALUE)
        CloseHandle(dev.disk);
    for (int i = 0; i < dev.vol_count; i++)
        CloseHandle(dev.vol[i]);
}

rawio_ssize_t rawio_pread(RawDevice dev, void *buf, size_t n, uint64_t offset) {
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.Offset     = (DWORD)(offset & 0xFFFFFFFFULL);
    ov.OffsetHigh = (DWORD)(offset >> 32);
    DWORD got = 0;
    if (!ReadFile(dev.disk, buf, (DWORD)n, &got, &ov)) return -1;
    return (rawio_ssize_t)got;
}

rawio_ssize_t rawio_pwrite(RawDevice dev, const void *buf, size_t n, uint64_t offset) {
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.Offset     = (DWORD)(offset & 0xFFFFFFFFULL);
    ov.OffsetHigh = (DWORD)(offset >> 32);
    DWORD wrote = 0;
    if (!WriteFile(dev.disk, buf, (DWORD)n, &wrote, &ov)) {
        fprintf(stderr, "\n[rawio] WriteFile failed at offset %llu: error %lu\n",
                (unsigned long long)offset, (unsigned long)GetLastError());
        return -1;
    }
    return (rawio_ssize_t)wrote;
}

void *rawio_alloc(size_t size) {
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

void rawio_free(void *ptr) {
    if (ptr) VirtualFree(ptr, 0, MEM_RELEASE);
}

double rawio_now_ms(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart * 1000.0 / (double)freq.QuadPart;
}

/* ════════════════════════════════════════════════════════
 *  LINUX
 * ════════════════════════════════════════════════════════ */
#else
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

RawDevice rawio_open_rw(const char *path) {
    return open(path, O_RDWR | O_DIRECT | O_SYNC);
}

RawDevice rawio_open_ro(const char *path) {
    return open(path, O_RDONLY | O_DIRECT);
}

void rawio_close(RawDevice dev) {
    if (dev >= 0) close(dev);
}

rawio_ssize_t rawio_pread(RawDevice dev, void *buf, size_t n, uint64_t offset) {
    return (rawio_ssize_t)pread(dev, buf, n, (off_t)offset);
}

rawio_ssize_t rawio_pwrite(RawDevice dev, const void *buf, size_t n, uint64_t offset) {
    return (rawio_ssize_t)pwrite(dev, buf, n, (off_t)offset);
}

void *rawio_alloc(size_t size) {
    void *p = NULL;
    if (posix_memalign(&p, RAWIO_SECTOR, size) != 0) return NULL;
    return p;
}

void rawio_free(void *ptr) { free(ptr); }

double rawio_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

#endif
