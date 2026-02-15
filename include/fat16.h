#ifndef FAT16_H
#define FAT16_H

#include "types.h"

#define FAT16_MAX_NAME     128
#define FAT16_MAX_PATH     256
#define FAT16_SECTOR_SIZE  512

/* Directory entry attributes */
#define FAT_ATTR_READONLY  0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME    0x08
#define FAT_ATTR_DIR       0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       0x0F

typedef struct {
    bool     mounted;
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint32_t total_sectors;
    uint16_t fat_size_sectors;
    uint32_t fat_start_lba;
    uint32_t root_dir_lba;
    uint32_t root_dir_sectors;
    uint32_t data_start_lba;
    uint32_t cluster_size;     /* bytes per cluster */
    uint32_t total_clusters;
    char     volume_label[12];
} fat16_info_t;

typedef struct {
    char     name[FAT16_MAX_NAME];  /* Decoded 8.3 or LFN */
    uint8_t  attr;
    uint32_t size;
    uint16_t first_cluster;
    uint16_t date, time;
    bool     is_dir;
} fat16_dirent_t;

/* Mount / unmount */
bool fat16_mount(void);
bool fat16_mount_drive(int drv);  /* Mount on specific ATA drive (0=hda, 1=hdb) */
int  fat16_get_drive_idx(void);   /* Which drive FAT16 is mounted on */
void fat16_unmount(void);
bool fat16_is_mounted(void);
fat16_info_t* fat16_get_info(void);

/* Directory listing: fills entries[], returns count (-1 on error) */
int fat16_list_dir(const char* path, fat16_dirent_t* entries, int max_entries);

/* Read file contents from disk into buf. Returns bytes read or -1 */
int32_t fat16_read_file(const char* path, void* buf, uint32_t max_size);

/* Write file to disk. Creates or overwrites. Returns bytes written or -1 */
int32_t fat16_write_file(const char* path, const void* data, uint32_t size);

/* Delete a file. Returns 0 on success */
int32_t fat16_delete_file(const char* path);

/* Create directory. Returns 0 on success */
int32_t fat16_mkdir(const char* path);

/* Get file size, -1 if not found */
int32_t fat16_file_size(const char* path);

/* Check if path exists and is directory */
bool fat16_is_dir(const char* path);

/* Print disk info */
void fat16_print_info(void);

/* Format a disk with FAT16 filesystem */
bool fat16_format(uint32_t total_sectors, const char* label);

#endif
