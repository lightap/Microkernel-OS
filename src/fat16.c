#include "fat16.h"
#include "ata.h"
#include "heap.h"
#include "vga.h"

/* Drive-aware I/O: macros route all ata calls through the selected drive */
static int fat16_drv = 0;
#define ata_read_sector(lba, buf)  ata_read_sector_drv(fat16_drv, lba, buf)
#define ata_write_sector(lba, buf) ata_write_sector_drv(fat16_drv, lba, buf)
#define ata_available()            ata_drive_present(fat16_drv)

/* FAT16 Boot Parameter Block (offset into sector 0) */
typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT12/16 extended */
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_serial;
    char     volume_label[11];
    char     fs_type[8];
} fat16_bpb_t;

/* On-disk directory entry (32 bytes) */
typedef struct __attribute__((packed)) {
    char     name[8];
    char     ext[3];
    uint8_t  attr;
    uint8_t  reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_hi;    /* Always 0 for FAT16 */
    uint16_t write_time;
    uint16_t write_date;
    uint16_t cluster_lo;
    uint32_t file_size;
} fat16_direntry_t;

/* VFAT Long File Name entry (32 bytes, same slot size as 8.3 entry) */
typedef struct __attribute__((packed)) {
    uint8_t  seq;        /* Sequence number (1-20, 0x40 = last) */
    uint16_t name1[5];   /* Chars 1-5 (UCS-2 LE) */
    uint8_t  attr;       /* Always 0x0F */
    uint8_t  type;       /* Always 0x00 */
    uint8_t  checksum;   /* Checksum of short name */
    uint16_t name2[6];   /* Chars 6-11 (UCS-2 LE) */
    uint16_t cluster;    /* Always 0x0000 */
    uint16_t name3[2];   /* Chars 12-13 (UCS-2 LE) */
} fat16_lfn_entry_t;

/* Extended dir entry with optional LFN */
typedef struct {
    fat16_direntry_t sfn;
    char lfn[FAT16_MAX_NAME]; /* Long name, or empty string */
} fat16_ext_entry_t;

static fat16_info_t fs_info;
static uint8_t sector_buf[FAT16_SECTOR_SIZE];

/* Forward declarations */
static bool strcasecmp83(const char name[8], const char ext[3], const char* filename);

/* ---- FAT table helpers ---- */
static uint16_t fat_read_entry(uint16_t cluster) {
    uint32_t offset = cluster * 2;
    uint32_t sector = fs_info.fat_start_lba + (offset / FAT16_SECTOR_SIZE);
    uint32_t idx = offset % FAT16_SECTOR_SIZE;
    if (!ata_read_sector(sector, sector_buf)) return 0xFFFF;
    return *(uint16_t*)(sector_buf + idx);
}

static void fat_write_entry(uint16_t cluster, uint16_t value) {
    uint32_t offset = cluster * 2;
    uint32_t sector = fs_info.fat_start_lba + (offset / FAT16_SECTOR_SIZE);
    uint32_t idx = offset % FAT16_SECTOR_SIZE;
    if (!ata_read_sector(sector, sector_buf)) return;
    *(uint16_t*)(sector_buf + idx) = value;
    ata_write_sector(sector, sector_buf);
    /* Write to second FAT too */
    if (fs_info.num_fats > 1) {
        uint32_t fat2_sector = sector + fs_info.fat_size_sectors;
        ata_write_sector(fat2_sector, sector_buf);
    }
}

static uint16_t fat_alloc_cluster(void) {
    for (uint16_t c = 2; c < fs_info.total_clusters + 2; c++) {
        if (fat_read_entry(c) == 0x0000) {
            fat_write_entry(c, 0xFFFF); /* Mark as end-of-chain */
            return c;
        }
    }
    return 0; /* Disk full */
}

static void fat_free_chain(uint16_t start) {
    uint16_t c = start;
    while (c >= 2 && c < 0xFFF8) {
        uint16_t next = fat_read_entry(c);
        fat_write_entry(c, 0x0000);
        c = next;
    }
}

static uint32_t cluster_to_lba(uint16_t cluster) {
    return fs_info.data_start_lba + (uint32_t)(cluster - 2) * fs_info.sectors_per_cluster;
}

/* ---- 8.3 name helpers ---- */
static void decode_83_name(const fat16_direntry_t* entry, char* out) {
    int pos = 0;
    /* Name part: strip trailing spaces */
    for (int i = 0; i < 8 && entry->name[i] != ' '; i++)
        out[pos++] = (entry->name[i] >= 'A' && entry->name[i] <= 'Z')
            ? entry->name[i] + 32 : entry->name[i];
    /* Extension */
    if (entry->ext[0] != ' ') {
        out[pos++] = '.';
        for (int i = 0; i < 3 && entry->ext[i] != ' '; i++)
            out[pos++] = (entry->ext[i] >= 'A' && entry->ext[i] <= 'Z')
                ? entry->ext[i] + 32 : entry->ext[i];
    }
    out[pos] = '\0';
}

static void encode_83_name(const char* filename, char name[8], char ext[3]) {
    memset(name, ' ', 8);
    memset(ext, ' ', 3);
    const char* dot = NULL;
    for (const char* p = filename; *p; p++)
        if (*p == '.') dot = p;

    int len = dot ? (int)(dot - filename) : (int)strlen(filename);
    if (len > 8) len = 8;
    for (int i = 0; i < len; i++)
        name[i] = toupper(filename[i]);

    if (dot) {
        int elen = strlen(dot + 1);
        if (elen > 3) elen = 3;
        for (int i = 0; i < elen; i++)
            ext[i] = toupper(dot[1 + i]);
    }
}

/* ---- LFN helpers ---- */

/* Compute 8.3 checksum for LFN validation */
static uint8_t lfn_checksum(const char name83[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)name83[i];
    return sum;
}

/* Check if a filename requires LFN (doesn't fit valid 8.3) */
static bool needs_lfn(const char* name) {
    const char* dot = NULL;
    int dotcount = 0;
    for (const char* p = name; *p; p++) {
        if (*p == '.') { dot = p; dotcount++; }
        /* LFN needed for lowercase, spaces, special chars, long names */
        if (*p >= 'a' && *p <= 'z') return true;
        if (*p == ' ' || *p == '+' || *p == ',' || *p == ';' ||
            *p == '=' || *p == '[' || *p == ']') return true;
    }
    if (dotcount > 1) return true;

    int baselen = dot ? (int)(dot - name) : (int)strlen(name);
    int extlen = dot ? (int)strlen(dot + 1) : 0;
    if (baselen > 8 || extlen > 3) return true;
    if (baselen == 0) return true;
    return false;
}

/* Extract 13 chars from an LFN entry into a buffer */
static void lfn_extract_chars(const fat16_lfn_entry_t* lfn, char out[13]) {
    int pos = 0;
    for (int i = 0; i < 5; i++) out[pos++] = (char)(lfn->name1[i] & 0xFF);
    for (int i = 0; i < 6; i++) out[pos++] = (char)(lfn->name2[i] & 0xFF);
    for (int i = 0; i < 2; i++) out[pos++] = (char)(lfn->name3[i] & 0xFF);
}

/* Generate a unique 8.3 short name with ~N suffix for LFN entries */
static void generate_short_name(const char* longname, char name[8], char ext[3], int seq) {
    memset(name, ' ', 8);
    memset(ext, ' ', 3);

    /* Find last dot for extension */
    const char* dot = NULL;
    for (const char* p = longname; *p; p++)
        if (*p == '.') dot = p;

    /* Fill extension */
    if (dot) {
        int elen = strlen(dot + 1);
        if (elen > 3) elen = 3;
        for (int i = 0; i < elen; i++)
            ext[i] = toupper(dot[1 + i]);
    }

    /* Fill base name (uppercase, skip invalid chars) */
    int baselen = dot ? (int)(dot - longname) : (int)strlen(longname);
    int bpos = 0;
    for (int i = 0; i < baselen && bpos < 6; i++) {
        char c = longname[i];
        if (c == ' ' || c == '.') continue;
        name[bpos++] = toupper(c);
    }
    if (bpos == 0) name[bpos++] = '_';

    /* Add ~N suffix */
    name[bpos++] = '~';
    if (seq < 10) {
        name[bpos] = '0' + seq;
    } else {
        name[bpos++] = '0' + (seq / 10);
        if (bpos < 8) name[bpos] = '0' + (seq % 10);
    }
}

/* Build LFN entries for a long name. Returns number of LFN entries needed.
 * lfn_entries[0] is the LAST sequence (stored first on disk, reversed order).
 * checksum is the checksum of the corresponding 8.3 name. */
static int build_lfn_entries(const char* longname, uint8_t checksum,
                              fat16_lfn_entry_t* lfn_entries, int max_entries) {
    int namelen = strlen(longname);
    int num_entries = (namelen + 12) / 13; /* Ceiling division */
    if (num_entries > max_entries || num_entries > 20) return 0;

    for (int n = 0; n < num_entries; n++) {
        fat16_lfn_entry_t* le = &lfn_entries[n];
        memset(le, 0xFF, sizeof(*le)); /* Fill with 0xFF (padding) */
        int seq_num = num_entries - n; /* Reverse order: last fragment first */
        le->seq = (uint8_t)seq_num;
        if (n == 0) le->seq |= 0x40; /* Mark as last LFN entry */
        le->attr = FAT_ATTR_LFN;
        le->type = 0;
        le->checksum = checksum;
        le->cluster = 0;

        /* Fill characters for this entry */
        int base = (seq_num - 1) * 13;
        for (int i = 0; i < 5; i++) {
            int ci = base + i;
            le->name1[i] = (ci < namelen) ? (uint16_t)(uint8_t)longname[ci] : (ci == namelen ? 0x0000 : 0xFFFF);
        }
        for (int i = 0; i < 6; i++) {
            int ci = base + 5 + i;
            le->name2[i] = (ci < namelen) ? (uint16_t)(uint8_t)longname[ci] : (ci == namelen ? 0x0000 : 0xFFFF);
        }
        for (int i = 0; i < 2; i++) {
            int ci = base + 11 + i;
            le->name3[i] = (ci < namelen) ? (uint16_t)(uint8_t)longname[ci] : (ci == namelen ? 0x0000 : 0xFFFF);
        }
    }
    return num_entries;
}

/* Find N consecutive free directory entry slots */
static bool find_free_dir_slots(uint16_t dir_cluster, int count,
                                 uint32_t* out_lba, uint32_t* out_offset) {
    uint8_t buf[FAT16_SECTOR_SIZE];
    int consecutive = 0;
    uint32_t first_lba = 0, first_off = 0;

    if (dir_cluster == 0) {
        /* Root directory */
        for (uint32_t s = 0; s < fs_info.root_dir_sectors; s++) {
            uint32_t lba = fs_info.root_dir_lba + s;
            if (!ata_read_sector(lba, buf)) return false;
            for (int i = 0; i < 16; i++) {
                fat16_direntry_t* e = (fat16_direntry_t*)(buf + i * 32);
                if (e->name[0] == 0x00 || (uint8_t)e->name[0] == 0xE5) {
                    if (consecutive == 0) { first_lba = lba; first_off = i * 32; }
                    consecutive++;
                    if (consecutive >= count) {
                        *out_lba = first_lba; *out_offset = first_off;
                        return true;
                    }
                } else {
                    consecutive = 0;
                }
            }
        }
    } else {
        uint16_t cluster = dir_cluster;
        while (cluster >= 2 && cluster < 0xFFF8) {
            uint32_t base_lba = cluster_to_lba(cluster);
            for (uint8_t s = 0; s < fs_info.sectors_per_cluster; s++) {
                uint32_t lba = base_lba + s;
                if (!ata_read_sector(lba, buf)) return false;
                for (int i = 0; i < 16; i++) {
                    fat16_direntry_t* e = (fat16_direntry_t*)(buf + i * 32);
                    if (e->name[0] == 0x00 || (uint8_t)e->name[0] == 0xE5) {
                        if (consecutive == 0) { first_lba = lba; first_off = i * 32; }
                        consecutive++;
                        if (consecutive >= count) {
                            *out_lba = first_lba; *out_offset = first_off;
                            return true;
                        }
                    } else {
                        consecutive = 0;
                    }
                }
            }
            cluster = fat_read_entry(cluster);
        }
    }
    return false;
}

/* Write LFN entries + SFN entry starting at the given lba/offset */
static bool write_dir_entries(uint32_t start_lba, uint32_t start_offset,
                               fat16_lfn_entry_t* lfn_ents, int lfn_count,
                               fat16_direntry_t* sfn_entry) {
    /* We need to write lfn_count + 1 consecutive 32-byte entries */
    uint8_t buf[FAT16_SECTOR_SIZE];
    uint32_t cur_lba = start_lba;
    uint32_t cur_off = start_offset;

    /* Read the starting sector */
    if (!ata_read_sector(cur_lba, buf)) return false;

    /* Write LFN entries first (they're already in on-disk order) */
    for (int i = 0; i < lfn_count; i++) {
        memcpy(buf + cur_off, &lfn_ents[i], 32);
        cur_off += 32;
        if (cur_off >= FAT16_SECTOR_SIZE) {
            ata_write_sector(cur_lba, buf);
            cur_lba++;
            cur_off = 0;
            if (!ata_read_sector(cur_lba, buf)) return false;
        }
    }

    /* Write SFN entry */
    memcpy(buf + cur_off, sfn_entry, 32);
    ata_write_sector(cur_lba, buf);
    return true;
}

/* ---- Path helpers ---- */
static void split_path(const char* path, char* dir_part, char* file_part) {
    /* Find last slash */
    const char* last_slash = NULL;
    for (const char* p = path; *p; p++)
        if (*p == '/') last_slash = p;

    if (!last_slash || last_slash == path) {
        /* Root directory */
        strcpy(dir_part, "/");
        strcpy(file_part, last_slash ? last_slash + 1 : path);
    } else {
        int dlen = last_slash - path;
        strncpy(dir_part, path, dlen);
        dir_part[dlen] = '\0';
        strcpy(file_part, last_slash + 1);
    }
}

/* ---- Directory reading ---- */

/* Read root directory entries into a buffer.
 * Returns the sector and offset where a specific entry was found, or -1 */
static int read_root_dir(fat16_ext_entry_t* entries, int max) {
    int count = 0;
    uint8_t buf[FAT16_SECTOR_SIZE];
    char lfn_buf[FAT16_MAX_NAME];
    int lfn_pos = 0;
    bool collecting_lfn = false;

    for (uint32_t s = 0; s < fs_info.root_dir_sectors && count < max; s++) {
        if (!ata_read_sector(fs_info.root_dir_lba + s, buf)) break;
        for (int i = 0; i < 16 && count < max; i++) {
            fat16_direntry_t* e = (fat16_direntry_t*)(buf + i * 32);
            if (e->name[0] == 0x00) return count;
            if ((uint8_t)e->name[0] == 0xE5) { collecting_lfn = false; continue; }
            if (e->attr == FAT_ATTR_LFN) {
                /* Collect LFN entry */
                fat16_lfn_entry_t* le = (fat16_lfn_entry_t*)(buf + i * 32);
                int seq = le->seq & 0x1F;
                if (le->seq & 0x40) {
                    /* First (last on disk) LFN fragment — start fresh */
                    memset(lfn_buf, 0, sizeof(lfn_buf));
                    collecting_lfn = true;
                }
                if (collecting_lfn && seq >= 1 && seq <= 20) {
                    int base = (seq - 1) * 13;
                    char tmp[13];
                    lfn_extract_chars(le, tmp);
                    for (int c = 0; c < 13 && base + c < FAT16_MAX_NAME - 1; c++) {
                        if (tmp[c] == '\0' || tmp[c] == (char)0xFF) break;
                        lfn_buf[base + c] = tmp[c];
                    }
                    /* Track length */
                    int end = base + 13;
                    if (end > lfn_pos) lfn_pos = end;
                }
                continue;
            }
            if (e->attr & FAT_ATTR_VOLUME) { collecting_lfn = false; continue; }
            /* Regular 8.3 entry — pair with any collected LFN */
            entries[count].sfn = *e;
            if (collecting_lfn && lfn_buf[0]) {
                /* Null-terminate */
                for (int c = 0; c < FAT16_MAX_NAME; c++) {
                    if (lfn_buf[c] == (char)0xFF) { lfn_buf[c] = '\0'; break; }
                }
                strncpy(entries[count].lfn, lfn_buf, FAT16_MAX_NAME - 1);
                entries[count].lfn[FAT16_MAX_NAME - 1] = '\0';
            } else {
                entries[count].lfn[0] = '\0';
            }
            collecting_lfn = false;
            lfn_pos = 0;
            memset(lfn_buf, 0, sizeof(lfn_buf));
            count++;
        }
    }
    return count;
}

static int read_subdir(uint16_t start_cluster, fat16_ext_entry_t* entries, int max) {
    int count = 0;
    uint16_t cluster = start_cluster;
    uint8_t buf[FAT16_SECTOR_SIZE];
    char lfn_buf[FAT16_MAX_NAME];
    int lfn_pos = 0;
    bool collecting_lfn = false;

    while (cluster >= 2 && cluster < 0xFFF8 && count < max) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint8_t s = 0; s < fs_info.sectors_per_cluster && count < max; s++) {
            if (!ata_read_sector(lba + s, buf)) return count;
            for (int i = 0; i < 16 && count < max; i++) {
                fat16_direntry_t* e = (fat16_direntry_t*)(buf + i * 32);
                if (e->name[0] == 0x00) return count;
                if ((uint8_t)e->name[0] == 0xE5) { collecting_lfn = false; continue; }
                if (e->attr == FAT_ATTR_LFN) {
                    fat16_lfn_entry_t* le = (fat16_lfn_entry_t*)(buf + i * 32);
                    int seq = le->seq & 0x1F;
                    if (le->seq & 0x40) {
                        memset(lfn_buf, 0, sizeof(lfn_buf));
                        collecting_lfn = true;
                    }
                    if (collecting_lfn && seq >= 1 && seq <= 20) {
                        int base = (seq - 1) * 13;
                        char tmp[13];
                        lfn_extract_chars(le, tmp);
                        for (int c = 0; c < 13 && base + c < FAT16_MAX_NAME - 1; c++) {
                            if (tmp[c] == '\0' || tmp[c] == (char)0xFF) break;
                            lfn_buf[base + c] = tmp[c];
                        }
                        int end = base + 13;
                        if (end > lfn_pos) lfn_pos = end;
                    }
                    continue;
                }
                if (e->attr & FAT_ATTR_VOLUME) { collecting_lfn = false; continue; }
                entries[count].sfn = *e;
                if (collecting_lfn && lfn_buf[0]) {
                    for (int c = 0; c < FAT16_MAX_NAME; c++) {
                        if (lfn_buf[c] == (char)0xFF) { lfn_buf[c] = '\0'; break; }
                    }
                    strncpy(entries[count].lfn, lfn_buf, FAT16_MAX_NAME - 1);
                    entries[count].lfn[FAT16_MAX_NAME - 1] = '\0';
                } else {
                    entries[count].lfn[0] = '\0';
                }
                collecting_lfn = false;
                lfn_pos = 0;
                memset(lfn_buf, 0, sizeof(lfn_buf));
                count++;
            }
        }
        cluster = fat_read_entry(cluster);
    }
    return count;
}

/* Navigate path components to find the directory cluster.
 * Returns 0 for root dir, or the cluster number, or 0xFFFF on error. */
/* Get the best name from an extended entry */
static const char* ext_name(fat16_ext_entry_t* e, char* buf83) {
    if (e->lfn[0]) return e->lfn;
    decode_83_name(&e->sfn, buf83);
    return buf83;
}

static uint16_t navigate_to_dir(const char* path) {
    if (!path || path[0] == '\0' || strcmp(path, "/") == 0) return 0;

    const char* p = path;
    if (*p == '/') p++;
    uint16_t current_cluster = 0; /* 0 = root */

    while (*p) {
        char component[FAT16_MAX_NAME];
        int ci = 0;
        while (*p && *p != '/' && ci < FAT16_MAX_NAME - 1) component[ci++] = *p++;
        component[ci] = '\0';
        if (*p == '/') p++;
        if (ci == 0) continue;

        fat16_ext_entry_t entries[128];
        int count;
        if (current_cluster == 0)
            count = read_root_dir(entries, 128);
        else
            count = read_subdir(current_cluster, entries, 128);

        bool found = false;
        for (int i = 0; i < count; i++) {
            char n83[13];
            const char* ename = ext_name(&entries[i], n83);
            if (strcasecmp83(entries[i].sfn.name, entries[i].sfn.ext, component) ||
                strcmp(ename, component) == 0) {
                if (!(entries[i].sfn.attr & FAT_ATTR_DIR)) return 0xFFFF;
                current_cluster = entries[i].sfn.cluster_lo;
                found = true;
                break;
            }
        }
        if (!found) return 0xFFFF;
    }
    return current_cluster;
}

/* Case-insensitive compare of 8.3 name against a filename */
static bool strcasecmp83(const char name[8], const char ext[3], const char* filename) {
    char decoded[13];
    /* Build decoded name from 8.3 */
    int pos = 0;
    for (int i = 0; i < 8 && name[i] != ' '; i++)
        decoded[pos++] = tolower(name[i]);
    if (ext[0] != ' ') {
        decoded[pos++] = '.';
        for (int i = 0; i < 3 && ext[i] != ' '; i++)
            decoded[pos++] = tolower(ext[i]);
    }
    decoded[pos] = '\0';

    /* Compare case-insensitively */
    const char* a = decoded;
    const char* b = filename;
    while (*a && *b) {
        if (tolower(*a) != tolower(*b)) return false;
        a++; b++;
    }
    return (*a == *b);
}

/* Find a specific file/dir entry in a directory.
 * Sets *out_entry on success. Also sets *out_dir_lba and *out_dir_offset
 * for modifying the entry on disk. Returns true if found. */
static bool find_entry_in_dir(uint16_t dir_cluster, const char* name,
                               fat16_direntry_t* out_entry,
                               uint32_t* out_dir_lba, uint32_t* out_dir_offset) {
    uint8_t buf[FAT16_SECTOR_SIZE];
    char lfn_buf[FAT16_MAX_NAME];
    bool collecting_lfn = false;

    /* Macro to scan one sector */
    #define SCAN_SECTOR(lba_val) do { \
        uint32_t _lba = (lba_val); \
        if (!ata_read_sector(_lba, buf)) return false; \
        for (int i = 0; i < 16; i++) { \
            fat16_direntry_t* e = (fat16_direntry_t*)(buf + i * 32); \
            if (e->name[0] == 0x00) return false; \
            if ((uint8_t)e->name[0] == 0xE5) { collecting_lfn = false; continue; } \
            if (e->attr == FAT_ATTR_LFN) { \
                fat16_lfn_entry_t* le = (fat16_lfn_entry_t*)(buf + i * 32); \
                int seq = le->seq & 0x1F; \
                if (le->seq & 0x40) { memset(lfn_buf, 0, sizeof(lfn_buf)); collecting_lfn = true; } \
                if (collecting_lfn && seq >= 1 && seq <= 20) { \
                    int base = (seq - 1) * 13; \
                    char tmp[13]; lfn_extract_chars(le, tmp); \
                    for (int c = 0; c < 13 && base+c < FAT16_MAX_NAME-1; c++) { \
                        if (tmp[c] == '\0' || tmp[c] == (char)0xFF) break; \
                        lfn_buf[base+c] = tmp[c]; \
                    } \
                } \
                continue; \
            } \
            if (e->attr & FAT_ATTR_VOLUME) { collecting_lfn = false; continue; } \
            /* Check LFN match */ \
            bool matched = false; \
            if (collecting_lfn && lfn_buf[0]) { \
                for (int c = 0; c < FAT16_MAX_NAME; c++) \
                    if (lfn_buf[c] == (char)0xFF) { lfn_buf[c] = '\0'; break; } \
                /* Case-insensitive LFN compare */ \
                const char* a = lfn_buf; const char* b = name; \
                bool lfn_match = true; \
                while (*a && *b) { \
                    if (tolower(*a) != tolower(*b)) { lfn_match = false; break; } \
                    a++; b++; \
                } \
                if (lfn_match && *a == *b) matched = true; \
            } \
            if (!matched) matched = strcasecmp83(e->name, e->ext, name); \
            collecting_lfn = false; memset(lfn_buf, 0, sizeof(lfn_buf)); \
            if (matched) { \
                *out_entry = *e; \
                if (out_dir_lba) *out_dir_lba = _lba; \
                if (out_dir_offset) *out_dir_offset = i * 32; \
                return true; \
            } \
        } \
    } while(0)

    if (dir_cluster == 0) {
        for (uint32_t s = 0; s < fs_info.root_dir_sectors; s++)
            SCAN_SECTOR(fs_info.root_dir_lba + s);
    } else {
        uint16_t cluster = dir_cluster;
        while (cluster >= 2 && cluster < 0xFFF8) {
            uint32_t base_lba = cluster_to_lba(cluster);
            for (uint8_t s = 0; s < fs_info.sectors_per_cluster; s++)
                SCAN_SECTOR(base_lba + s);
            cluster = fat_read_entry(cluster);
        }
    }
    #undef SCAN_SECTOR
    return false;
}

/* Find a free directory entry slot. Returns true and sets lba/offset. */
/* ---- Public API ---- */

bool fat16_mount(void) {
    memset(&fs_info, 0, sizeof(fs_info));
    if (!ata_available()) return false;

    /* Read boot sector */
    uint8_t boot[FAT16_SECTOR_SIZE];
    if (!ata_read_sector(0, boot)) return false;

    /* Check boot signature */
    if (boot[510] != 0x55 || boot[511] != 0xAA) return false;

    fat16_bpb_t* bpb = (fat16_bpb_t*)boot;

    /* Validate it looks like FAT16 */
    if (bpb->bytes_per_sector != 512) return false;
    if (bpb->num_fats == 0 || bpb->num_fats > 2) return false;
    if (bpb->fat_size_16 == 0) return false;
    if (bpb->root_entry_count == 0) return false;

    fs_info.bytes_per_sector = bpb->bytes_per_sector;
    fs_info.sectors_per_cluster = bpb->sectors_per_cluster;
    fs_info.reserved_sectors = bpb->reserved_sectors;
    fs_info.num_fats = bpb->num_fats;
    fs_info.root_entry_count = bpb->root_entry_count;
    fs_info.fat_size_sectors = bpb->fat_size_16;

    fs_info.total_sectors = bpb->total_sectors_16 ? bpb->total_sectors_16 : bpb->total_sectors_32;

    /* Calculate key LBA positions */
    fs_info.fat_start_lba = bpb->reserved_sectors;
    fs_info.root_dir_lba = fs_info.fat_start_lba + (uint32_t)bpb->num_fats * bpb->fat_size_16;
    fs_info.root_dir_sectors = ((uint32_t)bpb->root_entry_count * 32 + 511) / 512;
    fs_info.data_start_lba = fs_info.root_dir_lba + fs_info.root_dir_sectors;
    fs_info.cluster_size = (uint32_t)bpb->sectors_per_cluster * 512;

    uint32_t data_sectors = fs_info.total_sectors - fs_info.data_start_lba;
    fs_info.total_clusters = data_sectors / bpb->sectors_per_cluster;

    /* Copy volume label */
    memcpy(fs_info.volume_label, bpb->volume_label, 11);
    fs_info.volume_label[11] = '\0';
    int vl = 10;
    while (vl >= 0 && fs_info.volume_label[vl] == ' ')
        fs_info.volume_label[vl--] = '\0';

    fs_info.mounted = true;
    return true;
}

bool fat16_mount_drive(int drv) {
    fat16_drv = drv;
    return fat16_mount();
}

int fat16_get_drive_idx(void) { return fat16_drv; }

void fat16_unmount(void) {
    fs_info.mounted = false;
}

bool fat16_is_mounted(void)        { return fs_info.mounted; }
fat16_info_t* fat16_get_info(void) { return &fs_info; }

int fat16_list_dir(const char* path, fat16_dirent_t* entries, int max_entries) {
    if (!fs_info.mounted) return -1;

    uint16_t dir_cluster = navigate_to_dir(path);
    if (dir_cluster == 0xFFFF) return -1;

    fat16_ext_entry_t raw[128];
    int count;
    if (dir_cluster == 0)
        count = read_root_dir(raw, 128);
    else
        count = read_subdir(dir_cluster, raw, 128);

    int out = 0;
    for (int i = 0; i < count && out < max_entries; i++) {
        if (raw[i].sfn.name[0] == '.') continue; /* Skip . and .. */

        /* Use LFN if available, otherwise decode 8.3 */
        if (raw[i].lfn[0]) {
            strncpy(entries[out].name, raw[i].lfn, FAT16_MAX_NAME - 1);
            entries[out].name[FAT16_MAX_NAME - 1] = '\0';
        } else {
            decode_83_name(&raw[i].sfn, entries[out].name);
        }
        entries[out].attr = raw[i].sfn.attr;
        entries[out].size = raw[i].sfn.file_size;
        entries[out].first_cluster = raw[i].sfn.cluster_lo;
        entries[out].date = raw[i].sfn.write_date;
        entries[out].time = raw[i].sfn.write_time;
        entries[out].is_dir = (raw[i].sfn.attr & FAT_ATTR_DIR) ? true : false;
        out++;
    }
    return out;
}

int32_t fat16_read_file(const char* path, void* buf, uint32_t max_size) {
    if (!fs_info.mounted) return -1;

    /* Split into directory + filename */
    char dir_path[FAT16_MAX_PATH], filename[FAT16_MAX_NAME];
    split_path(path, dir_path, filename);
    if (filename[0] == '\0') return -1;

    uint16_t dir_cluster = navigate_to_dir(dir_path);
    if (dir_cluster == 0xFFFF) return -1;

    fat16_direntry_t entry;
    uint32_t dummy_lba, dummy_off;
    if (!find_entry_in_dir(dir_cluster, filename, &entry, &dummy_lba, &dummy_off))
        return -1;
    if (entry.attr & FAT_ATTR_DIR) return -1; /* Can't read a directory as file */

    uint32_t file_size = entry.file_size;
    uint32_t to_read = (file_size < max_size) ? file_size : max_size;
    uint32_t read_total = 0;
    uint16_t cluster = entry.cluster_lo;
    uint8_t* dst = (uint8_t*)buf;
    uint8_t sec_buf[FAT16_SECTOR_SIZE];

    while (cluster >= 2 && cluster < 0xFFF8 && read_total < to_read) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint8_t s = 0; s < fs_info.sectors_per_cluster && read_total < to_read; s++) {
            if (!ata_read_sector(lba + s, sec_buf)) return read_total;
            uint32_t chunk = to_read - read_total;
            if (chunk > 512) chunk = 512;
            memcpy(dst + read_total, sec_buf, chunk);
            read_total += chunk;
        }
        cluster = fat_read_entry(cluster);
    }
    return (int32_t)read_total;
}

int32_t fat16_write_file(const char* path, const void* data, uint32_t size) {
    if (!fs_info.mounted) return -1;

    char dir_path[FAT16_MAX_PATH], filename[FAT16_MAX_NAME];
    split_path(path, dir_path, filename);
    if (filename[0] == '\0') return -1;

    uint16_t dir_cluster = navigate_to_dir(dir_path);
    if (dir_cluster == 0xFFFF) return -1;

    /* Check if file already exists — if so, delete it first */
    fat16_direntry_t existing;
    uint32_t ex_lba, ex_off;
    if (find_entry_in_dir(dir_cluster, filename, &existing, &ex_lba, &ex_off)) {
        if (!(existing.attr & FAT_ATTR_DIR)) {
            fat_free_chain(existing.cluster_lo);
            /* Mark entry as deleted */
            uint8_t buf[FAT16_SECTOR_SIZE];
            ata_read_sector(ex_lba, buf);
            buf[ex_off] = 0xE5;
            ata_write_sector(ex_lba, buf);
        }
    }

    /* Allocate cluster chain for new file */
    uint32_t clusters_needed = (size + fs_info.cluster_size - 1) / fs_info.cluster_size;
    if (clusters_needed == 0) clusters_needed = 1;

    uint16_t first_cluster = 0;
    uint16_t prev_cluster = 0;
    for (uint32_t i = 0; i < clusters_needed; i++) {
        uint16_t c = fat_alloc_cluster();
        if (c == 0) {
            /* Out of space — free what we allocated */
            if (first_cluster) fat_free_chain(first_cluster);
            return -1;
        }
        if (i == 0) first_cluster = c;
        else fat_write_entry(prev_cluster, c);
        prev_cluster = c;
    }

    /* Write file data to clusters */
    const uint8_t* src = (const uint8_t*)data;
    uint32_t written = 0;
    uint16_t cluster = first_cluster;
    uint8_t sec_buf[FAT16_SECTOR_SIZE];

    while (cluster >= 2 && cluster < 0xFFF8 && written < size) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint8_t s = 0; s < fs_info.sectors_per_cluster && written < size; s++) {
            uint32_t chunk = size - written;
            if (chunk > 512) chunk = 512;
            memset(sec_buf, 0, 512);
            memcpy(sec_buf, src + written, chunk);
            if (!ata_write_sector(lba + s, sec_buf)) return written;
            written += chunk;
        }
        cluster = fat_read_entry(cluster);
    }

    /* Create directory entry (with optional LFN) */
    bool use_lfn = needs_lfn(filename);
    char sfn_name[8], sfn_ext[3];

    if (use_lfn) {
        generate_short_name(filename, sfn_name, sfn_ext, 1);
        /* Check for collisions and increment sequence */
        for (int seq = 1; seq <= 99; seq++) {
            generate_short_name(filename, sfn_name, sfn_ext, seq);
            fat16_direntry_t check;
            uint32_t cl, co;
            char check_name[13];
            int pos = 0;
            for (int i = 0; i < 8 && sfn_name[i] != ' '; i++)
                check_name[pos++] = sfn_name[i] + ('a' - 'A');
            if (sfn_ext[0] != ' ') {
                check_name[pos++] = '.';
                for (int i = 0; i < 3 && sfn_ext[i] != ' '; i++)
                    check_name[pos++] = sfn_ext[i] + ('a' - 'A');
            }
            check_name[pos] = '\0';
            if (!find_entry_in_dir(dir_cluster, check_name, &check, &cl, &co)) break;
        }
    } else {
        encode_83_name(filename, sfn_name, sfn_ext);
    }

    fat16_direntry_t new_sfn;
    memset(&new_sfn, 0, 32);
    memcpy(new_sfn.name, sfn_name, 8);
    memcpy(new_sfn.ext, sfn_ext, 3);
    new_sfn.attr = FAT_ATTR_ARCHIVE;
    new_sfn.cluster_lo = first_cluster;
    new_sfn.file_size = size;

    if (use_lfn) {
        /* Compute checksum of the 8.3 name */
        char name11[11];
        memcpy(name11, sfn_name, 8);
        memcpy(name11 + 8, sfn_ext, 3);
        uint8_t cksum = lfn_checksum(name11);

        fat16_lfn_entry_t lfn_ents[20];
        int lfn_count = build_lfn_entries(filename, cksum, lfn_ents, 20);
        if (lfn_count == 0) {
            fat_free_chain(first_cluster);
            return -1;
        }

        uint32_t slot_lba, slot_off;
        if (!find_free_dir_slots(dir_cluster, lfn_count + 1, &slot_lba, &slot_off)) {
            fat_free_chain(first_cluster);
            return -1;
        }
        if (!write_dir_entries(slot_lba, slot_off, lfn_ents, lfn_count, &new_sfn)) {
            fat_free_chain(first_cluster);
            return -1;
        }
    } else {
        uint32_t slot_lba, slot_off;
        if (!find_free_dir_slots(dir_cluster, 1, &slot_lba, &slot_off)) {
            fat_free_chain(first_cluster);
            return -1;
        }
        uint8_t dir_buf[FAT16_SECTOR_SIZE];
        ata_read_sector(slot_lba, dir_buf);
        memcpy(dir_buf + slot_off, &new_sfn, 32);
        ata_write_sector(slot_lba, dir_buf);
    }

    return (int32_t)written;
}

int32_t fat16_delete_file(const char* path) {
    if (!fs_info.mounted) return -1;

    char dir_path[FAT16_MAX_PATH], filename[FAT16_MAX_NAME];
    split_path(path, dir_path, filename);

    uint16_t dir_cluster = navigate_to_dir(dir_path);
    if (dir_cluster == 0xFFFF) return -1;

    fat16_direntry_t entry;
    uint32_t entry_lba, entry_off;
    if (!find_entry_in_dir(dir_cluster, filename, &entry, &entry_lba, &entry_off))
        return -1;

    /* Free cluster chain */
    if (entry.cluster_lo >= 2)
        fat_free_chain(entry.cluster_lo);

    /* Mark directory entry as deleted */
    uint8_t buf[FAT16_SECTOR_SIZE];
    ata_read_sector(entry_lba, buf);
    buf[entry_off] = 0xE5;
    ata_write_sector(entry_lba, buf);

    return 0;
}

int32_t fat16_mkdir(const char* path) {
    if (!fs_info.mounted) return -1;

    char dir_path[FAT16_MAX_PATH], dirname[FAT16_MAX_NAME];
    split_path(path, dir_path, dirname);
    if (dirname[0] == '\0') return -1;

    uint16_t parent_cluster = navigate_to_dir(dir_path);
    if (parent_cluster == 0xFFFF) return -1;

    /* Allocate cluster for new directory */
    uint16_t new_cluster = fat_alloc_cluster();
    if (new_cluster == 0) return -1;

    /* Zero out the cluster */
    uint8_t zero[FAT16_SECTOR_SIZE];
    memset(zero, 0, sizeof(zero));
    uint32_t lba = cluster_to_lba(new_cluster);
    for (uint8_t s = 0; s < fs_info.sectors_per_cluster; s++)
        ata_write_sector(lba + s, zero);

    /* Create . and .. entries */
    uint8_t dir_buf[FAT16_SECTOR_SIZE];
    memset(dir_buf, 0, sizeof(dir_buf));

    fat16_direntry_t* dot = (fat16_direntry_t*)dir_buf;
    memset(dot->name, ' ', 8); memset(dot->ext, ' ', 3);
    dot->name[0] = '.';
    dot->attr = FAT_ATTR_DIR;
    dot->cluster_lo = new_cluster;

    fat16_direntry_t* dotdot = (fat16_direntry_t*)(dir_buf + 32);
    memset(dotdot->name, ' ', 8); memset(dotdot->ext, ' ', 3);
    dotdot->name[0] = '.'; dotdot->name[1] = '.';
    dotdot->attr = FAT_ATTR_DIR;
    dotdot->cluster_lo = parent_cluster;

    ata_write_sector(lba, dir_buf);

    /* Add entry in parent directory (with optional LFN) */
    bool use_lfn = needs_lfn(dirname);
    char sfn_name[8], sfn_ext[3];

    if (use_lfn) {
        for (int seq = 1; seq <= 99; seq++) {
            generate_short_name(dirname, sfn_name, sfn_ext, seq);
            fat16_direntry_t check;
            uint32_t cl, co;
            char check_name[13];
            int pos = 0;
            for (int i = 0; i < 8 && sfn_name[i] != ' '; i++)
                check_name[pos++] = sfn_name[i] + ('a' - 'A');
            if (sfn_ext[0] != ' ') {
                check_name[pos++] = '.';
                for (int i = 0; i < 3 && sfn_ext[i] != ' '; i++)
                    check_name[pos++] = sfn_ext[i] + ('a' - 'A');
            }
            check_name[pos] = '\0';
            if (!find_entry_in_dir(parent_cluster, check_name, &check, &cl, &co)) break;
        }
    } else {
        encode_83_name(dirname, sfn_name, sfn_ext);
    }

    fat16_direntry_t entry;
    memset(&entry, 0, 32);
    memcpy(entry.name, sfn_name, 8);
    memcpy(entry.ext, sfn_ext, 3);
    entry.attr = FAT_ATTR_DIR;
    entry.cluster_lo = new_cluster;

    if (use_lfn) {
        char name11[11];
        memcpy(name11, sfn_name, 8);
        memcpy(name11 + 8, sfn_ext, 3);
        uint8_t cksum = lfn_checksum(name11);

        fat16_lfn_entry_t lfn_ents[20];
        int lfn_count = build_lfn_entries(dirname, cksum, lfn_ents, 20);
        if (lfn_count == 0) { fat_free_chain(new_cluster); return -1; }

        uint32_t slot_lba, slot_off;
        if (!find_free_dir_slots(parent_cluster, lfn_count + 1, &slot_lba, &slot_off)) {
            fat_free_chain(new_cluster); return -1;
        }
        if (!write_dir_entries(slot_lba, slot_off, lfn_ents, lfn_count, &entry)) {
            fat_free_chain(new_cluster); return -1;
        }
    } else {
        uint32_t slot_lba, slot_off;
        if (!find_free_dir_slots(parent_cluster, 1, &slot_lba, &slot_off)) {
            fat_free_chain(new_cluster); return -1;
        }
        ata_read_sector(slot_lba, dir_buf);
        memcpy(dir_buf + slot_off, &entry, 32);
        ata_write_sector(slot_lba, dir_buf);
    }

    return 0;
}

int32_t fat16_file_size(const char* path) {
    if (!fs_info.mounted) return -1;
    char dir_path[FAT16_MAX_PATH], filename[FAT16_MAX_NAME];
    split_path(path, dir_path, filename);
    uint16_t dir_cluster = navigate_to_dir(dir_path);
    if (dir_cluster == 0xFFFF) return -1;
    fat16_direntry_t entry;
    uint32_t d1, d2;
    if (!find_entry_in_dir(dir_cluster, filename, &entry, &d1, &d2)) return -1;
    return (int32_t)entry.file_size;
}

bool fat16_is_dir(const char* path) {
    if (!fs_info.mounted) return false;
    return navigate_to_dir(path) != 0xFFFF;
}

void fat16_print_info(void) {
    if (!fs_info.mounted) {
        kprintf("  FAT16: not mounted\n");
        return;
    }
    kprintf("  FAT16 Filesystem:\n");
    kprintf("    Label:    %s\n", fs_info.volume_label);
    kprintf("    Cluster:  %u bytes (%u sectors)\n",
            fs_info.cluster_size, fs_info.sectors_per_cluster);
    kprintf("    FATs:     %u x %u sectors\n", fs_info.num_fats, fs_info.fat_size_sectors);
    kprintf("    Root:     %u entries\n", fs_info.root_entry_count);
    kprintf("    Clusters: %u total\n", fs_info.total_clusters);
    kprintf("    Capacity: %u MB\n",
            (fs_info.total_clusters * fs_info.cluster_size) / (1024 * 1024));
}

bool fat16_format(uint32_t total_sectors, const char* label) {
    if (!ata_available()) return false;
    if (total_sectors < 4096) return false; /* Need at least ~2MB */

    /* Compute FAT16 parameters */
    uint8_t  spc;  /* sectors per cluster */
    uint32_t size_mb = total_sectors / 2048;

    if (size_mb <= 16)       spc = 4;   /* 2KB clusters */
    else if (size_mb <= 128) spc = 8;   /* 4KB clusters */
    else if (size_mb <= 256) spc = 16;  /* 8KB clusters */
    else if (size_mb <= 512) spc = 32;  /* 16KB clusters */
    else                     spc = 64;  /* 32KB clusters */

    uint16_t reserved = 1;
    uint8_t  num_fats = 2;
    uint16_t root_entries = 512;
    uint32_t root_sectors = (root_entries * 32 + 511) / 512;

    /* Calculate FAT size:
     * data_sectors = total - reserved - 2*fat_size - root_sectors
     * clusters = data_sectors / spc
     * fat_size = (clusters + 2) * 2 / 512, rounded up
     * This is circular, so iterate: */
    uint32_t fat_size = 1;
    for (int iter = 0; iter < 10; iter++) {
        uint32_t data_start = reserved + num_fats * fat_size + root_sectors;
        if (data_start >= total_sectors) return false;
        uint32_t data_sectors = total_sectors - data_start;
        uint32_t clusters = data_sectors / spc;
        uint32_t needed = ((clusters + 2) * 2 + 511) / 512;
        if (needed == fat_size) break;
        fat_size = needed;
    }

    /* Verify cluster count is in FAT16 range (4085 - 65524) */
    uint32_t data_start = reserved + num_fats * fat_size + root_sectors;
    uint32_t data_sectors = total_sectors - data_start;
    uint32_t clusters = data_sectors / spc;
    if (clusters < 4085 || clusters > 65524) {
        /* Adjust spc to fit FAT16 range */
        if (clusters < 4085) {
            kprintf("  Disk too small for FAT16 (%u clusters)\n", clusters);
            return false;
        }
        while (clusters > 65524 && spc < 128) {
            spc *= 2;
            clusters = data_sectors / spc;
        }
    }

    kprintf("  Formatting: %u MB, %u sectors/cluster, %u clusters\n",
            size_mb, spc, clusters);

    /* Write boot sector / BPB */
    uint8_t boot[512];
    memset(boot, 0, 512);
    boot[0] = 0xEB; boot[1] = 0x3C; boot[2] = 0x90; /* jmp short */
    memcpy(boot + 3, "MICROOS ", 8); /* OEM */

    fat16_bpb_t* bpb = (fat16_bpb_t*)boot;
    bpb->bytes_per_sector = 512;
    bpb->sectors_per_cluster = spc;
    bpb->reserved_sectors = reserved;
    bpb->num_fats = num_fats;
    bpb->root_entry_count = root_entries;
    if (total_sectors < 65536)
        bpb->total_sectors_16 = (uint16_t)total_sectors;
    else
        bpb->total_sectors_32 = total_sectors;
    bpb->media = 0xF8;
    bpb->fat_size_16 = (uint16_t)fat_size;
    bpb->sectors_per_track = 63;
    bpb->heads = 16;
    bpb->drive_number = 0x80;
    bpb->boot_sig = 0x29;
    bpb->volume_serial = 0x12345678;

    /* Volume label */
    memset(bpb->volume_label, ' ', 11);
    if (label) {
        int llen = strlen(label);
        if (llen > 11) llen = 11;
        for (int i = 0; i < llen; i++)
            bpb->volume_label[i] = toupper(label[i]);
    } else {
        memcpy(bpb->volume_label, "MICROKERNEL", 11);
    }
    memcpy(bpb->fs_type, "FAT16   ", 8);

    boot[510] = 0x55;
    boot[511] = 0xAA;

    if (!ata_write_sector(0, boot)) return false;
    kprintf("  Boot sector written\n");

    /* Write FAT tables — both copies */
    uint8_t fat_sec[512];
    for (int f = 0; f < num_fats; f++) {
        uint32_t fat_base = reserved + f * fat_size;
        for (uint32_t s = 0; s < fat_size; s++) {
            memset(fat_sec, 0, 512);
            if (s == 0) {
                /* First two entries are reserved */
                fat_sec[0] = 0xF8; fat_sec[1] = 0xFF; /* Media byte + 0xFF */
                fat_sec[2] = 0xFF; fat_sec[3] = 0xFF; /* End-of-chain marker */
            }
            if (!ata_write_sector(fat_base + s, fat_sec)) return false;
        }
    }
    kprintf("  FAT tables written (%u sectors x %u)\n", fat_size, num_fats);

    /* Clear root directory */
    uint32_t root_lba = reserved + num_fats * fat_size;
    uint8_t zero[512];
    memset(zero, 0, 512);
    for (uint32_t s = 0; s < root_sectors; s++) {
        if (!ata_write_sector(root_lba + s, zero)) return false;
    }

    /* Write volume label entry in root dir */
    uint8_t root_sec[512];
    memset(root_sec, 0, 512);
    fat16_direntry_t* vol = (fat16_direntry_t*)root_sec;
    memcpy(vol->name, bpb->volume_label, 8);
    memcpy(vol->ext, bpb->volume_label + 8, 3);
    vol->attr = FAT_ATTR_VOLUME;
    ata_write_sector(root_lba, root_sec);

    kprintf("  Root directory cleared (%u sectors)\n", root_sectors);
    kprintf("  Format complete: %s\n", label ? label : "MICROKERNEL");

    return true;
}
