#ifndef DEVICE_H
#define DEVICE_H

#include <stdint.h>

int    detect_device(const char *path, uint64_t *out_bytes);
char **list_removable_devices(int *count);
void   free_device_list(char **list, int count);

/* Returns 1 if path looks like a system/boot drive — must NOT be tested */
int    is_system_drive(const char *path);

/* Windows only: fills letters (e.g. "J") and label (e.g. "USB Drive")
 * for the given \\.\PhysicalDriveN path.
 * letters_out: buffer for comma-separated drive letters, e.g. "J"
 * label_out:   buffer for volume label of the first letter found      */
void   get_drive_letters(const char *phys_path,
                         char *letters_out, int letters_sz,
                         char *label_out,   int label_sz);

#endif /* DEVICE_H */
