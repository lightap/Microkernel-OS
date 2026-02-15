#ifndef NTFS_H
#define NTFS_H

#include "types.h"

#define NTFS_MAX_NAME      256
#define NTFS_MAX_PATH      512
#define NTFS_SECTOR_SIZE   512
#define NTFS_MFT_RECORD_SIZE 1024  /* Standard MFT record size */

/* MFT well-known file record numbers */
#define MFT_RECORD_MFT        0
#define MFT_RECORD_MFTMIRR    1
#define MFT_RECORD_LOGFILE    2
#define MFT_RECORD_VOLUME     3
#define MFT_RECORD_ATTRDEF    4
#define MFT_RECORD_ROOT       5   /* Root directory */
#define MFT_RECORD_BITMAP     6
#define MFT_RECORD_BOOT       7
#define MFT_RECORD_BADCLUS    8
#define MFT_RECORD_SECURE     9
#define MFT_RECORD_UPCASE    10
#define MFT_RECORD_EXTEND    11
#define MFT_FIRST_USER       16   /* First user file record */

/* Attribute types */
#define NTFS_ATTR_STANDARD_INFO   0x10
#define NTFS_ATTR_ATTRIBUTE_LIST  0x20
#define NTFS_ATTR_FILE_NAME       0x30
#define NTFS_ATTR_OBJECT_ID       0x40
#define NTFS_ATTR_SECURITY_DESC   0x50
#define NTFS_ATTR_VOLUME_NAME     0x60
#define NTFS_ATTR_VOLUME_INFO     0x70
#define NTFS_ATTR_DATA            0x80
#define NTFS_ATTR_INDEX_ROOT      0x90
#define NTFS_ATTR_INDEX_ALLOC     0xA0
#define NTFS_ATTR_BITMAP          0xB0
#define NTFS_ATTR_END             0xFFFFFFFF

/* File attribute flags (in $STANDARD_INFORMATION and $FILE_NAME) */
#define NTFS_FILE_ATTR_READONLY   0x0001
#define NTFS_FILE_ATTR_HIDDEN     0x0002
#define NTFS_FILE_ATTR_SYSTEM     0x0004
#define NTFS_FILE_ATTR_DIRECTORY  0x10000000
#define NTFS_FILE_ATTR_ARCHIVE    0x0020

/* MFT record flags */
#define MFT_RECORD_IN_USE     0x01
#define MFT_RECORD_IS_DIR     0x02

/* Index entry flags */
#define INDEX_ENTRY_SUBNODE   0x01
#define INDEX_ENTRY_LAST      0x02

typedef struct {
    bool     mounted;
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint32_t cluster_size;         /* bytes per cluster */
    uint64_t mft_cluster;          /* Starting cluster of $MFT */
    uint64_t mft_mirror_cluster;   /* Starting cluster of $MFTMirr */
    uint32_t mft_record_size;      /* Bytes per MFT record */
    uint32_t index_record_size;    /* Bytes per index record */
    uint64_t total_sectors;
    uint64_t volume_serial;
    char     volume_label[128];
    uint32_t total_mft_records;    /* Estimated from $MFT $DATA size */
} ntfs_info_t;

typedef struct {
    char     name[NTFS_MAX_NAME];
    uint32_t attr;                 /* File attribute flags */
    uint64_t size;                 /* File data size in bytes */
    uint64_t mft_ref;              /* MFT reference number */
    bool     is_dir;
    uint16_t date, time;           /* Approximate date/time from timestamp */
} ntfs_dirent_t;

/* Mount / unmount */
bool ntfs_mount(void);
bool ntfs_mount_drive(int drv);  /* Mount on specific ATA drive (0=hda, 1=hdb) */
int  ntfs_get_drive_idx(void);   /* Which drive NTFS is mounted on */
void ntfs_unmount(void);
bool ntfs_is_mounted(void);
ntfs_info_t* ntfs_get_info(void);

/* Directory listing: fills entries[], returns count (-1 on error) */
int ntfs_list_dir(const char* path, ntfs_dirent_t* entries, int max_entries);

/* Read file contents from disk into buf. Returns bytes read or -1 */
int32_t ntfs_read_file(const char* path, void* buf, uint32_t max_size);

/* Get file size, -1 if not found */
int32_t ntfs_file_size(const char* path);

/* Check if path exists and is directory */
bool ntfs_is_dir(const char* path);

/* Print filesystem info */
void ntfs_print_info(void);

/* Write file (creates or overwrites). Returns bytes written or -1 */
int32_t ntfs_write_file(const char* path, const void* data, uint32_t size);

/* Delete a file. Returns 0 on success, -1 on error */
int32_t ntfs_delete_file(const char* path);

/* Create directory. Returns 0 on success, -1 on error */
int32_t ntfs_mkdir(const char* path);

#endif
