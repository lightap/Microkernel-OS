#include "ata.h"
#include "vga.h"

/* Primary ATA bus ports */
#define ATA_DATA       0x1F0
#define ATA_ERROR      0x1F1
#define ATA_FEATURES   0x1F1
#define ATA_SECT_CNT   0x1F2
#define ATA_LBA_LO     0x1F3
#define ATA_LBA_MID    0x1F4
#define ATA_LBA_HI     0x1F5
#define ATA_DRIVE_HEAD 0x1F6
#define ATA_STATUS     0x1F7
#define ATA_COMMAND    0x1F7
#define ATA_CONTROL    0x3F6

#define ATA_SR_BSY     0x80
#define ATA_SR_DRDY    0x40
#define ATA_SR_DRQ     0x08
#define ATA_SR_ERR     0x01

#define ATA_CMD_READ_PIO   0x20
#define ATA_CMD_WRITE_PIO  0x30
#define ATA_CMD_IDENTIFY   0xEC
#define ATA_CMD_FLUSH      0xE7

/* Drive head base: master=0xA0, slave=0xB0 (bit 4 selects slave) */
#define ATA_DH_MASTER  0xA0
#define ATA_DH_SLAVE   0xB0
/* LBA mode adds 0x40: master=0xE0, slave=0xF0 */
#define ATA_DH_LBA_MASTER  0xE0
#define ATA_DH_LBA_SLAVE   0xF0

static ata_drive_t drives[ATA_MAX_DRIVES];
static int num_drives = 0;

static inline void ata_400ns_delay(void) {
    inb(ATA_CONTROL); inb(ATA_CONTROL);
    inb(ATA_CONTROL); inb(ATA_CONTROL);
}

static bool ata_wait_ready(void) {
    int timeout = 100000;
    while (timeout-- > 0) {
        uint8_t s = inb(ATA_STATUS);
        if (!(s & ATA_SR_BSY)) return true;
    }
    return false;
}

static bool ata_wait_drq(void) {
    int timeout = 100000;
    while (timeout-- > 0) {
        uint8_t s = inb(ATA_STATUS);
        if (s & ATA_SR_ERR) return false;
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ)) return true;
    }
    return false;
}

static void ata_copy_string(char* dst, const uint16_t* src, int words) {
    for (int i = 0; i < words; i++) {
        dst[i * 2]     = (char)(src[i] >> 8);
        dst[i * 2 + 1] = (char)(src[i] & 0xFF);
    }
    dst[words * 2] = '\0';
    int len = words * 2;
    while (len > 0 && dst[len - 1] == ' ') dst[--len] = '\0';
}

static bool ata_identify_drive(int idx) {
    ata_drive_t* d = &drives[idx];
    memset(d, 0, sizeof(*d));
    d->is_master = (idx == 0);

    uint8_t dh = (idx == 0) ? ATA_DH_MASTER : ATA_DH_SLAVE;

    /* Select drive */
    outb(ATA_DRIVE_HEAD, dh);
    ata_400ns_delay();

    if (!ata_wait_ready()) return false;

    uint8_t status = inb(ATA_STATUS);
    if (status == 0x00 || status == 0xFF) return false;

    /* IDENTIFY */
    outb(ATA_DRIVE_HEAD, dh);
    outb(ATA_SECT_CNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    ata_400ns_delay();
    status = inb(ATA_STATUS);
    if (status == 0) return false;

    if (!ata_wait_ready()) return false;

    /* Check ATA (not ATAPI) */
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HI) != 0) return false;

    if (!ata_wait_drq()) return false;

    uint16_t id[256];
    for (int i = 0; i < 256; i++)
        id[i] = inw(ATA_DATA);

    ata_copy_string(d->serial, &id[10], 10);
    ata_copy_string(d->model, &id[27], 20);

    d->sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    if (d->sectors == 0) {
        uint32_t c = id[1], h = id[3], s = id[6];
        d->sectors = c * h * s;
    }

    d->size_mb = d->sectors / 2048;
    d->present = true;
    return true;
}

void ata_init(void) {
    memset(drives, 0, sizeof(drives));
    num_drives = 0;

    /* Soft reset */
    outb(ATA_CONTROL, 0x04);
    ata_400ns_delay();
    outb(ATA_CONTROL, 0x00);
    ata_400ns_delay();

    /* Detect master (hda) */
    if (ata_identify_drive(0))
        num_drives++;

    /* Detect slave (hdb) */
    if (ata_identify_drive(1))
        num_drives++;
}

/* Legacy API — drive 0 */
bool         ata_available(void)     { return drives[0].present; }
ata_drive_t* ata_get_drive(void)     { return &drives[0]; }

/* Multi-drive API */
int          ata_drive_count(void)        { return num_drives; }
bool         ata_drive_present(int idx)   { return (idx >= 0 && idx < ATA_MAX_DRIVES) ? drives[idx].present : false; }
ata_drive_t* ata_get_drive_n(int idx)     { return (idx >= 0 && idx < ATA_MAX_DRIVES) ? &drives[idx] : NULL; }

/* Drive-aware sector read */
bool ata_read_sector_drv(int idx, uint32_t lba, void* buffer) {
    if (idx < 0 || idx >= ATA_MAX_DRIVES || !drives[idx].present) return false;
    if (lba >= drives[idx].sectors) return false;
    if (!ata_wait_ready()) return false;

    uint8_t dh = (idx == 0) ? ATA_DH_LBA_MASTER : ATA_DH_LBA_SLAVE;
    outb(ATA_DRIVE_HEAD, dh | ((lba >> 24) & 0x0F));
    outb(ATA_FEATURES, 0x00);
    outb(ATA_SECT_CNT, 1);
    outb(ATA_LBA_LO,  (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_READ_PIO);

    if (!ata_wait_drq()) return false;
    uint16_t* buf = (uint16_t*)buffer;
    for (int i = 0; i < 256; i++)
        buf[i] = inw(ATA_DATA);
    ata_400ns_delay();
    return true;
}

/* Drive-aware sector write */
bool ata_write_sector_drv(int idx, uint32_t lba, const void* buffer) {
    if (idx < 0 || idx >= ATA_MAX_DRIVES || !drives[idx].present) return false;
    if (lba >= drives[idx].sectors) return false;
    if (!ata_wait_ready()) return false;

    uint8_t dh = (idx == 0) ? ATA_DH_LBA_MASTER : ATA_DH_LBA_SLAVE;
    outb(ATA_DRIVE_HEAD, dh | ((lba >> 24) & 0x0F));
    outb(ATA_FEATURES, 0x00);
    outb(ATA_SECT_CNT, 1);
    outb(ATA_LBA_LO,  (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_WRITE_PIO);

    if (!ata_wait_drq()) return false;
    const uint16_t* buf = (const uint16_t*)buffer;
    for (int i = 0; i < 256; i++)
        outw(ATA_DATA, buf[i]);
    ata_400ns_delay();

    outb(ATA_COMMAND, ATA_CMD_FLUSH);
    ata_wait_ready();
    return true;
}

/* Legacy multi-sector (drive 0 only) */
bool ata_read_sectors(uint32_t lba, uint8_t count, void* buffer) {
    uint8_t* dst = (uint8_t*)buffer;
    for (int i = 0; i < count; i++) {
        if (!ata_read_sector_drv(0, lba + i, dst + i * ATA_SECTOR_SIZE))
            return false;
    }
    return true;
}

bool ata_write_sectors(uint32_t lba, uint8_t count, const void* buffer) {
    const uint8_t* src = (const uint8_t*)buffer;
    for (int i = 0; i < count; i++) {
        if (!ata_write_sector_drv(0, lba + i, src + i * ATA_SECTOR_SIZE))
            return false;
    }
    return true;
}

bool ata_read_sector(uint32_t lba, void* buffer) {
    return ata_read_sector_drv(0, lba, buffer);
}

bool ata_write_sector(uint32_t lba, const void* buffer) {
    return ata_write_sector_drv(0, lba, buffer);
}

void ata_print_info(void) {
    if (num_drives == 0) {
        kprintf("  No ATA disks detected.\n");
        kprintf("  Tip: qemu ... -hda disk.img -hdb disk2.img\n");
        return;
    }
    for (int i = 0; i < ATA_MAX_DRIVES; i++) {
        if (!drives[i].present) continue;
        kprintf("  ATA Disk %d (%s):\n", i, drives[i].is_master ? "hda" : "hdb");
        kprintf("    Model:   %s\n", drives[i].model);
        kprintf("    Serial:  %s\n", drives[i].serial);
        kprintf("    Sectors: %u\n", drives[i].sectors);
        kprintf("    Size:    %u MB\n", drives[i].size_mb);
    }
}
