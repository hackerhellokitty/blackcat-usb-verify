/*
 * debug_drives.c — standalone tool ดู PhysicalDrive ทั้งหมดพร้อม BusType
 * Build: gcc debug_drives.c -o debug_drives.exe -lwinioctl
 * หรือ:  gcc debug_drives.c -o debug_drives.exe
 */
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>

static const char *bus_name(STORAGE_BUS_TYPE t) {
    switch (t) {
        case BusTypeUnknown:   return "Unknown";
        case BusTypeScsi:      return "SCSI";
        case BusTypeAtapi:     return "ATAPI";
        case BusTypeAta:       return "ATA";
        case BusType1394:      return "1394";
        case BusTypeSsa:       return "SSA";
        case BusTypeFibre:     return "Fibre";
        case BusTypeUsb:       return "USB";
        case BusTypeRAID:      return "RAID";
        case BusTypeiScsi:     return "iSCSI";
        case BusTypeSas:       return "SAS";
        case BusTypeSata:      return "SATA";
        case BusTypeSd:        return "SD";
        case BusTypeMmc:       return "MMC";
        case BusTypeVirtual:   return "Virtual";
        case BusTypeFileBackedVirtual: return "FileBackedVirtual";
        case BusTypeSpaces:    return "Spaces";
        case BusTypeNvme:      return "NVMe";
        case BusTypeSCM:       return "SCM";
        case BusTypeUfs:       return "UFS";
        default:               return "Other";
    }
}

int main(void) {
    printf("%-30s  %-12s  %-10s  %-8s  %s\n",
           "Path", "BusType", "Removable", "Size(GB)", "Vendor/Product");
    printf("%-30s  %-12s  %-10s  %-8s  %s\n",
           "------------------------------", "------------",
           "----------", "--------", "--------------");

    for (int i = 0; i < 16; i++) {
        char path[64];
        snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%d", i);

        HANDLE h = CreateFileA(path,
                               GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;

        /* size */
        DISK_GEOMETRY_EX geo = {0};
        DWORD ret;
        DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                        NULL, 0, &geo, sizeof(geo), &ret, NULL);
        double size_gb = (double)geo.DiskSize.QuadPart / (1024.0*1024.0*1024.0);

        /* storage descriptor */
        STORAGE_PROPERTY_QUERY q = {StorageDeviceProperty, PropertyStandardQuery, {0}};
        DWORD needed = 0;
        DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                        &q, sizeof(q), NULL, 0, &needed, NULL);

        char vendor[32]  = "?";
        char product[64] = "?";
        STORAGE_BUS_TYPE bus = BusTypeUnknown;
        BOOLEAN removable    = FALSE;

        if (needed >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
            STORAGE_DEVICE_DESCRIPTOR *desc = malloc(needed);
            if (desc) {
                if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                                    &q, sizeof(q), desc, needed, &ret, NULL)) {
                    bus       = desc->BusType;
                    removable = desc->RemovableMedia;
                    if (desc->VendorIdOffset && desc->VendorIdOffset < needed)
                        snprintf(vendor, sizeof(vendor), "%s",
                                 (char*)desc + desc->VendorIdOffset);
                    if (desc->ProductIdOffset && desc->ProductIdOffset < needed)
                        snprintf(product, sizeof(product), "%s",
                                 (char*)desc + desc->ProductIdOffset);
                }
                free(desc);
            }
        }
        CloseHandle(h);

        /* trim trailing spaces */
        for (int j = (int)strlen(vendor)-1;  j >= 0 && vendor[j]  == ' '; j--) vendor[j]  = '\0';
        for (int j = (int)strlen(product)-1; j >= 0 && product[j] == ' '; j--) product[j] = '\0';

        printf("%-30s  %-12s  %-10s  %8.2f  %s %s\n",
               path,
               bus_name(bus),
               removable ? "YES" : "no",
               size_gb,
               vendor, product);
    }
    return 0;
}
