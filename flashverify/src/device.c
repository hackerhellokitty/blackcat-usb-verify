#include "device.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>   /* IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                           IOCTL_STORAGE_QUERY_PROPERTY,
                           IOCTL_STORAGE_GET_DEVICE_NUMBER */
#include <setupapi.h>   /* SetupDiGetClassDevs — USB disk enumeration */
#include <devguid.h>    /* GUID_DEVCLASS_DISKDRIVE */
#include <ntddstor.h>   /* GUID_DEVINTERFACE_DISK */

/* ── detect_device ──────────────────────────────────────────────────────── */

int detect_device(const char *path, uint64_t *out_bytes) {
    HANDLE h = CreateFileA(path,
                           GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;

    DISK_GEOMETRY_EX geo;
    DWORD returned;
    BOOL ok = DeviceIoControl(h,
                              IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                              NULL, 0,
                              &geo, sizeof(geo),
                              &returned, NULL);
    CloseHandle(h);
    if (!ok) return -1;

    *out_bytes = (uint64_t)geo.DiskSize.QuadPart;
    return 0;
}

/* ── is_system_drive (Windows) ──────────────────────────────────────────── */

int is_system_drive(const char *path) {
    /* PhysicalDrive0 คือ boot drive เกือบทุกกรณี */
    if (strstr(path, "PhysicalDrive0") || strstr(path, "physicaldrive0"))
        return 1;

    /* ตรวจว่า drive ที่เลือกเป็น physical disk ที่ mount Windows อยู่ */
    char win_root[MAX_PATH] = {0};
    if (GetWindowsDirectoryA(win_root, sizeof(win_root)) == 0)
        return 0;

    char vol_path[16];
    snprintf(vol_path, sizeof(vol_path), "\\\\.\\%c:", win_root[0]);

    HANDLE h = CreateFileA(vol_path, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    STORAGE_DEVICE_NUMBER sdn;
    DWORD returned;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                              NULL, 0, &sdn, sizeof(sdn), &returned, NULL);
    CloseHandle(h);
    if (!ok) return 0;

    char sys_path[64];
    snprintf(sys_path, sizeof(sys_path),
             "\\\\.\\PhysicalDrive%lu", (unsigned long)sdn.DeviceNumber);

    return (_stricmp(path, sys_path) == 0) ? 1 : 0;
}

/* ── get_physical_drive_number_for_device ───────────────────────────────
 * ใช้ IOCTL_STORAGE_GET_DEVICE_NUMBER เพื่อหาเลข PhysicalDrive จาก
 * device instance path ที่ได้จาก SetupAPI
 * ─────────────────────────────────────────────────────────────────────── */
static int get_drive_number(const char *dev_path) {
    HANDLE h = CreateFileA(dev_path,
                           0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;

    STORAGE_DEVICE_NUMBER sdn;
    DWORD ret;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                              NULL, 0, &sdn, sizeof(sdn), &ret, NULL);
    CloseHandle(h);
    if (!ok) return -1;
    return (int)sdn.DeviceNumber;
}

/* ── list_removable_devices (Windows) ──────────────────────────────────── *
 *
 * Strategy A: SetupAPI — enumerate USB disk class (ไม่ต้องการ Admin)
 * Strategy B: fallback วน PhysicalDrive0-15 + IOCTL_STORAGE_QUERY_PROPERTY
 *             (ต้องการ Admin แต่จะทำงานถ้า A ล้มเหลว)
 *
 * ─────────────────────────────────────────────────────────────────────── */

char **list_removable_devices(int *count) {
    char **list = malloc(32 * sizeof(char *));
    if (!list) { *count = 0; return NULL; }
    int n = 0;

    /* ── Strategy A: SetupAPI GUID_DEVCLASS_DISKDRIVE ── */
    HDEVINFO dev_info = SetupDiGetClassDevsA(
        &GUID_DEVCLASS_DISKDRIVE,
        NULL, NULL,
        DIGCF_PRESENT);

    if (dev_info != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA dev_data;
        dev_data.cbSize = sizeof(SP_DEVINFO_DATA);

        for (DWORD idx = 0;
             SetupDiEnumDeviceInfo(dev_info, idx, &dev_data);
             idx++) {

            /* อ่าน hardware ID — USB disk มี "USBSTOR" ใน HardwareID */
            char hw_id[512] = {0};
            SetupDiGetDeviceRegistryPropertyA(
                dev_info, &dev_data,
                SPDRP_HARDWAREID,
                NULL, (PBYTE)hw_id, sizeof(hw_id) - 1, NULL);

            int is_usb = (strstr(hw_id, "USBSTOR") != NULL ||
                          strstr(hw_id, "usbstor") != NULL);
            if (!is_usb) continue;

            /* หา device interface path เพื่อเปิด handle */
            SP_DEVICE_INTERFACE_DATA iface;
            iface.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

            HDEVINFO iface_info = SetupDiGetClassDevsA(
                &GUID_DEVINTERFACE_DISK,
                NULL, NULL,
                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (iface_info == INVALID_HANDLE_VALUE) continue;

            /* หา interface ที่ตรงกับ devInst นี้ */
            SP_DEVINFO_DATA iface_dev;
            iface_dev.cbSize = sizeof(SP_DEVINFO_DATA);
            BOOL found = FALSE;
            for (DWORD j = 0;
                 SetupDiEnumDeviceInfo(iface_info, j, &iface_dev);
                 j++) {
                if (iface_dev.DevInst == dev_data.DevInst) {
                    found = TRUE;
                    break;
                }
            }

            if (found && SetupDiEnumDeviceInterfaces(
                    iface_info, &iface_dev,
                    &GUID_DEVINTERFACE_DISK, 0, &iface)) {

                DWORD needed2 = 0;
                SetupDiGetDeviceInterfaceDetailA(
                    iface_info, &iface, NULL, 0, &needed2, NULL);

                SP_DEVICE_INTERFACE_DETAIL_DATA_A *detail =
                    (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)malloc(needed2);
                if (detail) {
                    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
                    if (SetupDiGetDeviceInterfaceDetailA(
                            iface_info, &iface, detail,
                            needed2, NULL, NULL)) {

                        int drv_num = get_drive_number(detail->DevicePath);
                        if (drv_num >= 0 && n < 32) {
                            char phys[64];
                            snprintf(phys, sizeof(phys),
                                     "\\\\.\\PhysicalDrive%d", drv_num);
                            if (!is_system_drive(phys))
                                list[n++] = strdup(phys);
                        }
                    }
                    free(detail);
                }
            }
            SetupDiDestroyDeviceInfoList(iface_info);
        }
        SetupDiDestroyDeviceInfoList(dev_info);
    }

    /* ── Strategy B: fallback ถ้า SetupAPI ไม่ได้ผล ── */
    if (n == 0) {
        for (int i = 0; i < 16 && n < 32; i++) {
            char path[64];
            snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%d", i);
            if (is_system_drive(path)) continue;

            HANDLE h = CreateFileA(path, GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE) continue;

            STORAGE_PROPERTY_QUERY q = {StorageDeviceProperty,
                                        PropertyStandardQuery, {0}};
            DWORD needed = 0;
            DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                            &q, sizeof(q), NULL, 0, &needed, NULL);

            BOOL removable = FALSE;
            if (needed >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
                STORAGE_DEVICE_DESCRIPTOR *desc = malloc(needed);
                if (desc) {
                    DWORD ret;
                    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                                        &q, sizeof(q),
                                        desc, needed, &ret, NULL)) {
                        removable = desc->RemovableMedia
                                 || desc->BusType == BusTypeUsb
                                 || desc->BusType == BusTypeSd
                                 || desc->BusType == BusTypeMmc;
                    }
                    free(desc);
                }
            }
            CloseHandle(h);
            if (removable) list[n++] = strdup(path);
        }
    }

    *count = n;
    return list;
}

void get_drive_letters(const char *phys_path,
                       char *letters_out, int letters_sz,
                       char *label_out,   int label_sz) {
    letters_out[0] = '\0';
    label_out[0]   = '\0';

    /* parse PhysicalDriveN number */
    int drv_num = -1;
    const char *p = phys_path;
    while (*p) {
        if (_strnicmp(p, "PhysicalDrive", 13) == 0) {
            drv_num = atoi(p + 13);
            break;
        }
        p++;
    }
    if (drv_num < 0) return;

    int first = 1;
    for (char c = 'A'; c <= 'Z'; c++) {
        char vol[8];
        snprintf(vol, sizeof(vol), "\\\\.\\%c:", c);
        HANDLE h = CreateFileA(vol, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;

        STORAGE_DEVICE_NUMBER sdn;
        DWORD ret;
        BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                                  NULL, 0, &sdn, sizeof(sdn), &ret, NULL);
        CloseHandle(h);
        if (!ok || (int)sdn.DeviceNumber != drv_num) continue;

        /* append letter */
        if (!first && letters_sz > 2) {
            strncat(letters_out, ",", letters_sz - strlen(letters_out) - 1);
        }
        char ltr[2] = { c, '\0' };
        strncat(letters_out, ltr, letters_sz - strlen(letters_out) - 1);
        first = 0;

        /* get volume label for first letter */
        if (label_out[0] == '\0') {
            char root[8];
            snprintf(root, sizeof(root), "%c:\\", c);
            GetVolumeInformationA(root, label_out, label_sz,
                                  NULL, NULL, NULL, NULL, 0);
        }
    }
}

#else
/* ──────────────────────────────────────────────────────────────────────────
 * Linux
 * ──────────────────────────────────────────────────────────────────────── */
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <dirent.h>

int detect_device(const char *path, uint64_t *out_bytes) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    uint64_t size = 0;
    if (ioctl(fd, BLKGETSIZE64, &size) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    *out_bytes = size;
    return 0;
}

int is_system_drive(const char *path) {
    if (strcmp(path, "/dev/sda")      == 0) return 1;
    if (strcmp(path, "/dev/nvme0n1")  == 0) return 1;

    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;

    char line[256];
    size_t pathlen = strlen(path);
    while (fgets(line, sizeof(line), f)) {
        char dev[128], mnt[128];
        if (sscanf(line, "%127s %127s", dev, mnt) != 2) continue;
        if (strncmp(dev, path, pathlen) == 0 && strcmp(mnt, "/") == 0) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

char **list_removable_devices(int *count) {
    char **list = malloc(32 * sizeof(char *));
    if (!list) { *count = 0; return NULL; }
    int n = 0;

    DIR *d = opendir("/sys/block");
    if (!d) { *count = 0; return list; }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && n < 32) {
        if (entry->d_name[0] == '.') continue;

        char rem_path[256];
        snprintf(rem_path, sizeof(rem_path),
                 "/sys/block/%s/removable", entry->d_name);

        FILE *f = fopen(rem_path, "r");
        if (!f) continue;
        char val[4] = {0};
        fgets(val, sizeof(val), f);
        fclose(f);
        if (val[0] != '1') continue;

        char dev_path[64];
        snprintf(dev_path, sizeof(dev_path), "/dev/%s", entry->d_name);
        list[n++] = strdup(dev_path);
    }
    closedir(d);

    *count = n;
    return list;
}

#endif /* _WIN32 */

void free_device_list(char **list, int count) {
    if (!list) return;
    for (int i = 0; i < count; i++) free(list[i]);
    free(list);
}
