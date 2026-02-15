#ifndef ATA_H
#define ATA_H

#include "types.h"

#define ATA_SECTOR_SIZE 512
#define ATA_MAX_DRIVES  2

typedef struct {
    bool     present;
    bool     is_master;
    uint32_t sectors;
    uint32_t size_mb;
    char     model[41];
    char     serial[21];
} ata_drive_t;

void         ata_init(void);

/* Legacy single-drive API (drive 0 = primary master) */
bool         ata_available(void);
ata_drive_t* ata_get_drive(void);

/* Multi-drive API */
int          ata_drive_count(void);
bool         ata_drive_present(int idx);
ata_drive_t* ata_get_drive_n(int idx);

/* Drive-aware sector I/O (0=master/hda, 1=slave/hdb) */
bool ata_read_sector_drv(int idx, uint32_t lba, void* buffer);
bool ata_write_sector_drv(int idx, uint32_t lba, const void* buffer);

/* Legacy sector I/O (drive 0) */
bool ata_read_sectors(uint32_t lba, uint8_t count, void* buffer);
bool ata_write_sectors(uint32_t lba, uint8_t count, const void* buffer);
bool ata_read_sector(uint32_t lba, void* buffer);
bool ata_write_sector(uint32_t lba, const void* buffer);

void ata_print_info(void);

#endif
