/*
 * ntfs.c - NTFS filesystem driver for MicroKernel
 *
 * Supports reading and writing files/directories on NTFS-formatted ATA disks.
 * Write support: file creation, overwrite, deletion, mkdir.
 * No journaling (writes go directly, $LogFile is not updated).
 *
 * References:
 *   - NTFS Documentation by Richard Russon & Yuval Fledel
 *   - Linux ntfs3 / ntfs-3g source
 */

#include "ntfs.h"
#include "ata.h"
#include "heap.h"
#include "vga.h"

/* Drive-aware I/O: macros route all ata calls through the selected drive */
static int ntfs_drv = 0;
#define ata_read_sector(lba, buf)  ata_read_sector_drv(ntfs_drv, lba, buf)
#define ata_write_sector(lba, buf) ata_write_sector_drv(ntfs_drv, lba, buf)
#define ata_available()            ata_drive_present(ntfs_drv)

/* ================================================================
 * On-disk structures (all packed, little-endian)
 * ================================================================ */

typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];
    char     oem_id[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  zero1[3];
    uint16_t unused1;
    uint8_t  media;
    uint16_t zero2;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t unused2;
    uint32_t unused3;
    uint64_t total_sectors;
    uint64_t mft_lcn;
    uint64_t mft_mirr_lcn;
    int8_t   clusters_per_mft;
    uint8_t  pad1[3];
    int8_t   clusters_per_index;
    uint8_t  pad2[3];
    uint64_t volume_serial;
    uint32_t checksum;
} ntfs_boot_sector_t;

typedef struct __attribute__((packed)) {
    char     magic[4];
    uint16_t update_seq_offset;
    uint16_t update_seq_count;
    uint64_t logfile_seq;
    uint16_t seq_number;
    uint16_t hard_link_count;
    uint16_t first_attr_offset;
    uint16_t flags;
    uint32_t used_size;
    uint32_t alloc_size;
    uint64_t base_record_ref;
    uint16_t next_attr_id;
    uint16_t pad;
    uint32_t record_number;
} mft_record_header_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t length;
    uint8_t  non_resident;
    uint8_t  name_length;
    uint16_t name_offset;
    uint16_t flags;
    uint16_t attr_id;
} attr_header_t;

typedef struct __attribute__((packed)) {
    attr_header_t header;
    uint32_t value_length;
    uint16_t value_offset;
    uint8_t  indexed;
    uint8_t  pad;
} attr_resident_t;

typedef struct __attribute__((packed)) {
    attr_header_t header;
    uint64_t start_vcn;
    uint64_t end_vcn;
    uint16_t run_offset;
    uint16_t compression_unit;
    uint32_t pad;
    uint64_t alloc_size;
    uint64_t real_size;
    uint64_t init_size;
} attr_nonresident_t;

typedef struct __attribute__((packed)) {
    uint64_t parent_ref;
    uint64_t creation_time;
    uint64_t modification_time;
    uint64_t mft_modification_time;
    uint64_t access_time;
    uint64_t alloc_size;
    uint64_t real_size;
    uint32_t flags;
    uint32_t reparse;
    uint8_t  name_length;
    uint8_t  name_type;
} filename_attr_t;

typedef struct __attribute__((packed)) {
    uint32_t attr_type;
    uint32_t collation_rule;
    uint32_t index_record_size;
    uint8_t  clusters_per_index;
    uint8_t  pad[3];
    uint32_t entries_offset;
    uint32_t total_entries_size;
    uint32_t alloc_entries_size;
    uint32_t flags;
} index_root_t;

typedef struct __attribute__((packed)) {
    char     magic[4];
    uint16_t update_seq_offset;
    uint16_t update_seq_count;
    uint64_t logfile_seq;
    uint64_t vcn;
    uint32_t entries_offset;
    uint32_t total_entries_size;
    uint32_t alloc_entries_size;
    uint32_t flags;
} index_record_header_t;

typedef struct __attribute__((packed)) {
    uint64_t mft_ref;
    uint16_t entry_length;
    uint16_t content_length;
    uint32_t flags;
} index_entry_t;

typedef struct __attribute__((packed)) {
    uint64_t creation_time;
    uint64_t modification_time;
    uint64_t mft_modification_time;
    uint64_t access_time;
    uint32_t flags;
    uint32_t max_versions;
    uint32_t version;
    uint32_t class_id;
} std_info_attr_t;

/* ================================================================
 * Static state
 * ================================================================ */

static ntfs_info_t fs_info;
static uint8_t sector_buf[NTFS_SECTOR_SIZE];
static uint8_t mft_buf[NTFS_MFT_RECORD_SIZE];

/* Cached bitmaps for write support */
static uint8_t* cluster_bitmap = NULL;
static uint32_t cluster_bitmap_bytes = 0;
static uint32_t total_data_clusters = 0;

static uint8_t* mft_bitmap = NULL;
static uint32_t mft_bitmap_bytes = 0;

typedef struct { uint64_t vcn, lcn, length; } data_run_t;
#define MAX_DATA_RUNS 256
typedef struct { data_run_t runs[MAX_DATA_RUNS]; int count; uint64_t total_clusters; } run_list_t;

static run_list_t cluster_bmp_runs;
static run_list_t mft_bmp_runs;
static bool mft_bmp_resident;

/* ================================================================
 * 64-bit division helpers (no libgcc)
 * ================================================================ */

static uint64_t div64_32(uint64_t n, uint32_t d) {
    if (d == 0) return 0;
    uint32_t hi = (uint32_t)(n >> 32), lo = (uint32_t)n;
    if (hi == 0) return lo / d;
    uint32_t q_hi = hi / d, rem = hi % d, q_lo;
    __asm__ volatile ("divl %2" : "=a"(q_lo), "=d"(rem) : "r"(d), "a"(lo), "d"(rem));
    return ((uint64_t)q_hi << 32) | q_lo;
}

static uint32_t mod64_32(uint64_t n, uint32_t d) {
    if (d == 0) return 0;
    uint32_t hi = (uint32_t)(n >> 32), lo = (uint32_t)n;
    if (hi == 0) return lo % d;
    uint32_t rem = hi % d, q_lo;
    __asm__ volatile ("divl %2" : "=a"(q_lo), "=d"(rem) : "r"(d), "a"(lo), "d"(rem));
    return rem;
}

/* ================================================================
 * Low-level I/O
 * ================================================================ */

static uint64_t cluster_to_offset(uint64_t c) { return c * fs_info.cluster_size; }
static uint32_t cluster_to_lba(uint64_t c)    { return (uint32_t)(c * fs_info.sectors_per_cluster); }

static bool read_cluster(uint64_t c, void* buf) {
    uint32_t lba = cluster_to_lba(c);
    uint8_t* dst = (uint8_t*)buf;
    for (uint32_t i = 0; i < fs_info.sectors_per_cluster; i++)
        if (!ata_read_sector(lba + i, dst + i * NTFS_SECTOR_SIZE)) return false;
    return true;
}

static bool write_cluster(uint64_t c, const void* buf) {
    uint32_t lba = cluster_to_lba(c);
    const uint8_t* src = (const uint8_t*)buf;
    for (uint32_t i = 0; i < fs_info.sectors_per_cluster; i++)
        if (!ata_write_sector(lba + i, src + i * NTFS_SECTOR_SIZE)) return false;
    return true;
}

static bool apply_fixup(uint8_t* rec, uint32_t rec_size) {
    mft_record_header_t* h = (mft_record_header_t*)rec;
    uint16_t uso = h->update_seq_offset, usc = h->update_seq_count;
    if ((uint32_t)uso + (uint32_t)usc * 2 > rec_size) return false;
    uint16_t* usa = (uint16_t*)(rec + uso);
    uint16_t sv = usa[0];
    for (uint16_t i = 1; i < usc; i++) {
        uint32_t p = i * NTFS_SECTOR_SIZE - 2;
        if (p + 2 > rec_size) break;
        uint16_t* s = (uint16_t*)(rec + p);
        if (*s != sv) return false;
        *s = usa[i];
    }
    return true;
}

static bool prepare_fixup(uint8_t* rec, uint32_t rec_size) {
    mft_record_header_t* h = (mft_record_header_t*)rec;
    uint16_t uso = h->update_seq_offset, usc = h->update_seq_count;
    if ((uint32_t)uso + (uint32_t)usc * 2 > rec_size) return false;
    uint16_t* usa = (uint16_t*)(rec + uso);
    usa[0]++;
    if (usa[0] == 0) usa[0] = 1;
    uint16_t sv = usa[0];
    for (uint16_t i = 1; i < usc; i++) {
        uint32_t p = i * NTFS_SECTOR_SIZE - 2;
        if (p + 2 > rec_size) break;
        uint16_t* s = (uint16_t*)(rec + p);
        usa[i] = *s;
        *s = sv;
    }
    return true;
}

static bool read_mft_record(uint64_t rn) {
    uint64_t off = cluster_to_offset(fs_info.mft_cluster) + rn * fs_info.mft_record_size;
    uint32_t lba = (uint32_t)(off / NTFS_SECTOR_SIZE);
    uint32_t secs = fs_info.mft_record_size / NTFS_SECTOR_SIZE;
    for (uint32_t i = 0; i < secs; i++)
        if (!ata_read_sector(lba + i, mft_buf + i * NTFS_SECTOR_SIZE)) return false;
    if (memcmp(mft_buf, "FILE", 4) != 0) return false;
    return apply_fixup(mft_buf, fs_info.mft_record_size);
}

static bool write_mft_record(uint64_t rn, uint8_t* rec) {
    if (!prepare_fixup(rec, fs_info.mft_record_size)) return false;
    uint64_t off = cluster_to_offset(fs_info.mft_cluster) + rn * fs_info.mft_record_size;
    uint32_t lba = (uint32_t)(off / NTFS_SECTOR_SIZE);
    uint32_t secs = fs_info.mft_record_size / NTFS_SECTOR_SIZE;
    for (uint32_t i = 0; i < secs; i++)
        if (!ata_write_sector(lba + i, rec + i * NTFS_SECTOR_SIZE)) return false;
    return true;
}

/* ================================================================
 * Attribute helpers
 * ================================================================ */

static attr_header_t* find_attribute(uint8_t* rec, uint32_t type, attr_header_t* prev) {
    mft_record_header_t* h = (mft_record_header_t*)rec;
    uint32_t off = prev ? (uint32_t)((uint8_t*)prev - rec) + prev->length : h->first_attr_offset;
    while (off + sizeof(attr_header_t) <= fs_info.mft_record_size) {
        attr_header_t* a = (attr_header_t*)(rec + off);
        if (a->type == NTFS_ATTR_END || a->type == 0) return NULL;
        if (a->length == 0 || a->length > fs_info.mft_record_size - off) return NULL;
        if (a->type == type) return a;
        off += a->length;
    }
    return NULL;
}

static void* get_resident_data(attr_header_t* a, uint32_t* len) {
    if (a->non_resident) return NULL;
    attr_resident_t* r = (attr_resident_t*)a;
    if (len) *len = r->value_length;
    return (uint8_t*)a + r->value_offset;
}

/* ================================================================
 * Data run decode / read / write
 * ================================================================ */

static bool parse_data_runs(attr_header_t* a, run_list_t* rl) {
    if (!a->non_resident) return false;
    attr_nonresident_t* nr = (attr_nonresident_t*)a;
    uint8_t* rd = (uint8_t*)a + nr->run_offset, *re = (uint8_t*)a + a->length;
    rl->count = 0; rl->total_clusters = 0;
    int64_t prev = 0;
    while (rd < re && *rd && rl->count < MAX_DATA_RUNS) {
        uint8_t hdr = *rd++;
        uint8_t lb = hdr & 0x0F, ob = (hdr >> 4) & 0x0F;
        if (!lb) break;
        if (rd + lb + ob > re) break;
        uint64_t len = 0;
        for (int i = 0; i < lb; i++) len |= (uint64_t)(*rd++) << (i * 8);
        int64_t ofs = 0;
        if (ob) {
            for (int i = 0; i < ob; i++) ofs |= (uint64_t)(*rd++) << (i * 8);
            if (ofs & ((int64_t)1 << (ob * 8 - 1)))
                ofs |= ~(((int64_t)1 << (ob * 8)) - 1);
        }
        if (!ob) { rl->total_clusters += len; continue; }
        int64_t lcn = prev + ofs; prev = lcn;
        data_run_t* r = &rl->runs[rl->count++];
        r->vcn = rl->total_clusters; r->lcn = (uint64_t)lcn; r->length = len;
        rl->total_clusters += len;
    }
    return rl->count > 0;
}

static int32_t read_data_runs(run_list_t* rl, void* buf, uint32_t max, uint64_t real) {
    uint32_t want = (max < (uint32_t)real) ? max : (uint32_t)real;
    uint8_t* dst = (uint8_t*)buf;
    uint32_t done = 0;
    uint8_t* cb = kmalloc(fs_info.cluster_size);
    if (!cb) return -1;
    for (int i = 0; i < rl->count && done < want; i++)
        for (uint64_t c = 0; c < rl->runs[i].length && done < want; c++) {
            if (!read_cluster(rl->runs[i].lcn + c, cb)) { kfree(cb); return -1; }
            uint32_t ch = want - done; if (ch > fs_info.cluster_size) ch = fs_info.cluster_size;
            memcpy(dst + done, cb, ch); done += ch;
        }
    kfree(cb);
    return (int32_t)done;
}

static int32_t write_data_runs(run_list_t* rl, const void* buf, uint32_t size) {
    const uint8_t* src = (const uint8_t*)buf;
    uint32_t done = 0;
    uint8_t* cb = kmalloc(fs_info.cluster_size);
    if (!cb) return -1;
    for (int i = 0; i < rl->count && done < size; i++)
        for (uint64_t c = 0; c < rl->runs[i].length && done < size; c++) {
            uint32_t ch = size - done; if (ch > fs_info.cluster_size) ch = fs_info.cluster_size;
            if (ch < fs_info.cluster_size) {
                memset(cb, 0, fs_info.cluster_size);
                memcpy(cb, src + done, ch);
                if (!write_cluster(rl->runs[i].lcn + c, cb)) { kfree(cb); return -1; }
            } else {
                if (!write_cluster(rl->runs[i].lcn + c, src + done)) { kfree(cb); return -1; }
            }
            done += ch;
        }
    kfree(cb);
    return (int32_t)done;
}

/* ================================================================
 * String helpers
 * ================================================================ */

static void utf16le_to_ascii(const uint16_t* s, int len, char* d, int ds) {
    int i; for (i = 0; i < len && i < ds - 1; i++) d[i] = (s[i] < 128) ? (char)s[i] : '?'; d[i] = '\0';
}
static void ascii_to_utf16le(const char* s, uint16_t* d, int mx) {
    int i; for (i = 0; s[i] && i < mx; i++) d[i] = (uint16_t)(uint8_t)s[i];
}
static bool strcaseeq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == *b;
}
static void split_path(const char* path, char* dir, char* file) {
    const char* ls = NULL;
    for (const char* p = path; *p; p++) if (*p == '/') ls = p;
    if (!ls || ls == path) { strcpy(dir, "/"); strcpy(file, ls ? ls + 1 : path); }
    else { int dl = (int)(ls - path); strncpy(dir, path, dl); dir[dl] = '\0'; strcpy(file, ls + 1); }
}

/* ================================================================
 * File name / data size helpers
 * ================================================================ */

static int64_t get_data_size(uint8_t* rec) {
    attr_header_t* a = find_attribute(rec, NTFS_ATTR_DATA, NULL);
    if (!a) return 0;
    if (a->non_resident) { attr_nonresident_t* nr = (attr_nonresident_t*)a; return (int64_t)nr->real_size; }
    else { uint32_t vl; get_resident_data(a, &vl); return (int64_t)vl; }
}

/* ================================================================
 * Directory traversal
 * ================================================================ */

typedef bool (*index_entry_cb_t)(index_entry_t* ie, void* ctx);

static int walk_index_entries(uint8_t* base, uint32_t size, index_entry_cb_t cb, void* ctx) {
    int cnt = 0; uint32_t off = 0;
    while (off + sizeof(index_entry_t) <= size) {
        index_entry_t* ie = (index_entry_t*)(base + off);
        if (ie->entry_length == 0 || off + ie->entry_length > size) break;
        if (ie->flags & INDEX_ENTRY_LAST) break;
        if (ie->content_length >= sizeof(filename_attr_t)) { if (!cb(ie, ctx)) return cnt; cnt++; }
        off += ie->entry_length;
    }
    return cnt;
}

typedef struct { ntfs_dirent_t* entries; int max, count; } dir_list_ctx_t;

static bool dir_list_callback(index_entry_t* ie, void* cp) {
    dir_list_ctx_t* c = (dir_list_ctx_t*)cp;
    if (c->count >= c->max) return false;
    filename_attr_t* fn = (filename_attr_t*)((uint8_t*)ie + sizeof(index_entry_t));
    if (fn->name_type == 2) return true;
    ntfs_dirent_t* de = &c->entries[c->count];
    utf16le_to_ascii((uint16_t*)((uint8_t*)fn + sizeof(filename_attr_t)), fn->name_length, de->name, NTFS_MAX_NAME);
    if (strcmp(de->name, ".") == 0 || strcmp(de->name, "..") == 0 || de->name[0] == '$') return true;
    de->mft_ref = ie->mft_ref & 0x0000FFFFFFFFFFFFULL;
    de->attr = fn->flags; de->size = fn->real_size;
    de->is_dir = (fn->flags & NTFS_FILE_ATTR_DIRECTORY) ? true : false;
    de->date = 0; de->time = 0;
    if (fn->modification_time) {
        uint64_t s = div64_32(fn->modification_time, 10000000);
        uint64_t us = (s > 11644473600ULL) ? s - 11644473600ULL : 0;
        uint32_t d = (uint32_t)div64_32(us, 86400), dt = (uint32_t)mod64_32(us, 86400);
        de->time = (uint16_t)((dt / 3600) * 100 + (dt % 3600) / 60);
        uint32_t y = 1970 + d / 365, m = (d % 365) / 30 + 1;
        if (m > 12) m = 12;
        de->date = (uint16_t)(y * 100 + m);
    }
    c->count++; return true;
}

typedef struct { const char* name; uint64_t ref; bool found, is_dir; } dir_lookup_ctx_t;

static bool dir_lookup_callback(index_entry_t* ie, void* cp) {
    dir_lookup_ctx_t* c = (dir_lookup_ctx_t*)cp;
    filename_attr_t* fn = (filename_attr_t*)((uint8_t*)ie + sizeof(index_entry_t));
    if (fn->name_type == 2) return true;
    char nm[NTFS_MAX_NAME];
    utf16le_to_ascii((uint16_t*)((uint8_t*)fn + sizeof(filename_attr_t)), fn->name_length, nm, NTFS_MAX_NAME);
    if (strcaseeq(nm, c->name)) {
        c->ref = ie->mft_ref & 0x0000FFFFFFFFFFFFULL; c->found = true;
        c->is_dir = (fn->flags & NTFS_FILE_ATTR_DIRECTORY) ? true : false; return false;
    }
    return true;
}

static int walk_directory(uint8_t* dr, index_entry_cb_t cb, void* ctx) {
    int total = 0;
    attr_header_t* ira = find_attribute(dr, NTFS_ATTR_INDEX_ROOT, NULL);
    if (!ira) return -1;
    uint32_t irl; index_root_t* ir = (index_root_t*)get_resident_data(ira, &irl);
    if (!ir || irl < sizeof(index_root_t)) return -1;
    total += walk_index_entries((uint8_t*)ir + 0x10 + ir->entries_offset, ir->total_entries_size, cb, ctx);
    if (!(ir->flags & 0x01)) return total;

    attr_header_t* iaa = find_attribute(dr, NTFS_ATTR_INDEX_ALLOC, NULL);
    if (!iaa || !iaa->non_resident) return total;
    run_list_t rl; if (!parse_data_runs(iaa, &rl)) return total;
    uint32_t irs = fs_info.index_record_size;
    uint8_t* ib = kmalloc(irs); if (!ib) return total;

    for (int r = 0; r < rl.count; r++)
        for (uint64_t c = 0; c < rl.runs[r].length; c++) {
            uint32_t lba = cluster_to_lba(rl.runs[r].lcn + c);
            uint32_t rpc = fs_info.cluster_size / irs; if (!rpc) rpc = 1;
            for (uint32_t ri = 0; ri < rpc; ri++) {
                uint32_t ro = ri * irs, rlba = lba + ro / NTFS_SECTOR_SIZE;
                uint32_t rs = irs / NTFS_SECTOR_SIZE;
                bool ok = true;
                for (uint32_t s = 0; s < rs; s++)
                    if (!ata_read_sector(rlba + s, ib + s * NTFS_SECTOR_SIZE)) { ok = false; break; }
                if (!ok || memcmp(ib, "INDX", 4) != 0 || !apply_fixup(ib, irs)) continue;
                index_record_header_t* irh = (index_record_header_t*)ib;
                total += walk_index_entries(ib + 0x18 + irh->entries_offset, irh->total_entries_size, cb, ctx);
            }
        }
    kfree(ib); return total;
}

/* ================================================================
 * Path resolution
 * ================================================================ */

static uint64_t resolve_path(const char* path) {
    if (!fs_info.mounted) return (uint64_t)-1;
    uint64_t cur = MFT_RECORD_ROOT;
    if (path[0] == '/' && path[1] == '\0') return cur;
    const char* p = path; if (*p == '/') p++;
    char comp[NTFS_MAX_NAME];
    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < NTFS_MAX_NAME - 1) comp[i++] = *p++;
        comp[i] = '\0'; if (*p == '/') p++;
        if (!comp[0]) continue;
        if (!read_mft_record(cur)) return (uint64_t)-1;
        if (!(((mft_record_header_t*)mft_buf)->flags & MFT_RECORD_IN_USE)) return (uint64_t)-1;
        dir_lookup_ctx_t ctx = { comp, 0, false, false };
        walk_directory(mft_buf, dir_lookup_callback, &ctx);
        if (!ctx.found) return (uint64_t)-1;
        cur = ctx.ref;
    }
    return cur;
}

/* ================================================================
 * Bitmap / allocation management
 * ================================================================ */

static uint8_t* load_bitmap(run_list_t* rl, uint32_t sz) {
    uint8_t* b = kcalloc(1, sz); if (!b) return NULL;
    if (read_data_runs(rl, b, sz, sz) < 0) { kfree(b); return NULL; }
    return b;
}

static bool flush_bitmap(run_list_t* rl, const uint8_t* b, uint32_t sz) {
    return write_data_runs(rl, b, sz) >= 0;
}

static uint64_t alloc_clusters(uint32_t count) {
    if (!cluster_bitmap || !count) return 0;
    uint32_t fs = 0, fc = 0;
    for (uint32_t c = 2; c < total_data_clusters; c++) {
        uint32_t bi = c >> 3;
        if (bi >= cluster_bitmap_bytes) break;
        if (!(cluster_bitmap[bi] & (1 << (c & 7)))) {
            if (!fc) fs = c;
            fc++;
            if (fc >= count) {
                for (uint32_t i = fs; i < fs + count; i++) cluster_bitmap[i >> 3] |= (1 << (i & 7));
                flush_bitmap(&cluster_bmp_runs, cluster_bitmap, cluster_bitmap_bytes);
                return fs;
            }
        } else fc = 0;
    }
    return 0;
}

static void free_clusters(uint64_t start, uint32_t count) {
    if (!cluster_bitmap) return;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t c = (uint32_t)start + i;
        if ((c >> 3) < cluster_bitmap_bytes) cluster_bitmap[c >> 3] &= ~(1 << (c & 7));
    }
    flush_bitmap(&cluster_bmp_runs, cluster_bitmap, cluster_bitmap_bytes);
}

static uint32_t alloc_mft_record(void) {
    if (!mft_bitmap) return 0;
    for (uint32_t i = MFT_FIRST_USER; (i >> 3) < mft_bitmap_bytes; i++) {
        if (!(mft_bitmap[i >> 3] & (1 << (i & 7)))) {
            mft_bitmap[i >> 3] |= (1 << (i & 7));
            if (mft_bmp_resident) {
                if (read_mft_record(MFT_RECORD_MFT)) {
                    attr_header_t* ba = find_attribute(mft_buf, NTFS_ATTR_BITMAP, NULL);
                    if (ba) { uint32_t bl; uint8_t* bd = (uint8_t*)get_resident_data(ba, &bl);
                              if (bd && bl >= mft_bitmap_bytes) memcpy(bd, mft_bitmap, mft_bitmap_bytes);
                              write_mft_record(MFT_RECORD_MFT, mft_buf); }
                }
            } else flush_bitmap(&mft_bmp_runs, mft_bitmap, mft_bitmap_bytes);
            return i;
        }
    }
    return 0;
}

static void free_mft_record(uint32_t rn) {
    if (!mft_bitmap || rn < MFT_FIRST_USER) return;
    if ((rn >> 3) < mft_bitmap_bytes) {
        mft_bitmap[rn >> 3] &= ~(1 << (rn & 7));
        if (mft_bmp_resident) {
            if (read_mft_record(MFT_RECORD_MFT)) {
                attr_header_t* ba = find_attribute(mft_buf, NTFS_ATTR_BITMAP, NULL);
                if (ba) { uint32_t bl; uint8_t* bd = (uint8_t*)get_resident_data(ba, &bl);
                          if (bd) memcpy(bd, mft_bitmap, mft_bitmap_bytes);
                          write_mft_record(MFT_RECORD_MFT, mft_buf); }
            }
        } else flush_bitmap(&mft_bmp_runs, mft_bitmap, mft_bitmap_bytes);
    }
}

/* ================================================================
 * Data run encoding
 * ================================================================ */

static int encode_data_run(uint8_t* out, uint64_t lcn, uint64_t length) {
    uint8_t lb = 0; { uint64_t v = length; while (v) { lb++; v >>= 8; } }
    if (!lb) lb = 1;
    uint8_t ob = 0; { uint64_t v = lcn; while (v) { ob++; v >>= 8; } }
    if (!ob) ob = 1;
    /* Extra byte if top bit set (would be interpreted as negative) */
    if (lcn && ((uint8_t)(lcn >> ((ob - 1) * 8)) & 0x80)) ob++;
    int p = 0;
    out[p++] = (ob << 4) | lb;
    for (int i = 0; i < lb; i++) out[p++] = (uint8_t)(length >> (i * 8));
    for (int i = 0; i < ob; i++) out[p++] = (uint8_t)(lcn >> (i * 8));
    return p;
}

/* ================================================================
 * MFT record construction
 * ================================================================ */

static uint32_t build_file_record(uint8_t* rec, uint32_t mft_num,
                                   uint64_t parent_ref, const char* name,
                                   bool is_dir, const void* data, uint32_t data_size,
                                   uint64_t data_cluster, uint32_t data_cc) {
    uint32_t rs = fs_info.mft_record_size;
    memset(rec, 0, rs);
    mft_record_header_t* h = (mft_record_header_t*)rec;
    memcpy(h->magic, "FILE", 4);
    h->update_seq_offset = 0x30;
    h->update_seq_count = (uint16_t)(rs / NTFS_SECTOR_SIZE + 1);
    h->seq_number = 1; h->hard_link_count = 1;
    h->first_attr_offset = 0x38;
    h->flags = MFT_RECORD_IN_USE | (is_dir ? MFT_RECORD_IS_DIR : 0);
    h->alloc_size = rs; h->record_number = mft_num;
    ((uint16_t*)(rec + 0x30))[0] = 1; /* USA seq */

    uint32_t pos = 0x38;
    uint16_t aid = 0;
    int nlen = strlen(name);

    /* $STANDARD_INFORMATION (0x10) */
    { uint32_t bs = 48, as = (sizeof(attr_resident_t) + bs + 7) & ~7;
      attr_resident_t* r = (attr_resident_t*)(rec + pos);
      r->header.type = NTFS_ATTR_STANDARD_INFO; r->header.length = as;
      r->header.attr_id = aid++; r->value_length = bs; r->value_offset = sizeof(attr_resident_t);
      std_info_attr_t* si = (std_info_attr_t*)(rec + pos + sizeof(attr_resident_t));
      si->flags = is_dir ? 0x10000000 : NTFS_FILE_ATTR_ARCHIVE;
      pos += as; }

    /* $FILE_NAME (0x30) */
    { uint32_t fbs = sizeof(filename_attr_t) + nlen * 2;
      uint32_t as = (sizeof(attr_resident_t) + fbs + 7) & ~7;
      attr_resident_t* r = (attr_resident_t*)(rec + pos);
      r->header.type = NTFS_ATTR_FILE_NAME; r->header.length = as;
      r->header.attr_id = aid++; r->value_length = fbs;
      r->value_offset = sizeof(attr_resident_t); r->indexed = 1;
      filename_attr_t* fn = (filename_attr_t*)(rec + pos + sizeof(attr_resident_t));
      fn->parent_ref = parent_ref | ((uint64_t)1 << 48);
      fn->alloc_size = is_dir ? 0 : ((uint64_t)data_cc * fs_info.cluster_size);
      fn->real_size = is_dir ? 0 : data_size;
      fn->flags = is_dir ? NTFS_FILE_ATTR_DIRECTORY : NTFS_FILE_ATTR_ARCHIVE;
      fn->name_length = (uint8_t)nlen; fn->name_type = 3;
      ascii_to_utf16le(name, (uint16_t*)((uint8_t*)fn + sizeof(filename_attr_t)), nlen);
      pos += as; }

    if (is_dir) {
        /* $INDEX_ROOT (0x90) for empty dir */
        uint32_t ee = 16; /* last-entry sentinel */
        uint32_t irbs = sizeof(index_root_t) + ee;
        /* Account for "$I30" name (4 UTF-16 chars = 8 bytes) */
        uint32_t name_sz = 8;
        uint32_t val_off = sizeof(attr_resident_t) + name_sz;
        uint32_t as = (val_off + irbs + 7) & ~7;
        attr_resident_t* r = (attr_resident_t*)(rec + pos);
        r->header.type = NTFS_ATTR_INDEX_ROOT; r->header.length = as;
        r->header.non_resident = 0; r->header.name_length = 4;
        r->header.name_offset = sizeof(attr_resident_t);
        r->header.attr_id = aid++; r->value_length = irbs; r->value_offset = val_off;
        uint16_t* in = (uint16_t*)(rec + pos + sizeof(attr_resident_t));
        in[0]='$'; in[1]='I'; in[2]='3'; in[3]='0';
        index_root_t* ir = (index_root_t*)(rec + pos + val_off);
        ir->attr_type = NTFS_ATTR_FILE_NAME; ir->collation_rule = 1;
        ir->index_record_size = fs_info.index_record_size;
        ir->clusters_per_index = 1; ir->entries_offset = 0x10;
        ir->total_entries_size = ee; ir->alloc_entries_size = ee;
        index_entry_t* end = (index_entry_t*)((uint8_t*)ir + 0x10 + 0x10);
        end->entry_length = (uint16_t)ee; end->flags = INDEX_ENTRY_LAST;
        pos += as;
    } else {
        /* $DATA (0x80) */
        uint32_t max_res = rs - pos - 32;
        if (data_size <= max_res - sizeof(attr_resident_t) && data_cluster == 0) {
            uint32_t as = (sizeof(attr_resident_t) + data_size + 7) & ~7;
            attr_resident_t* r = (attr_resident_t*)(rec + pos);
            r->header.type = NTFS_ATTR_DATA; r->header.length = as;
            r->header.attr_id = aid++; r->value_length = data_size;
            r->value_offset = sizeof(attr_resident_t);
            if (data && data_size) memcpy(rec + pos + sizeof(attr_resident_t), data, data_size);
            pos += as;
        } else {
            uint8_t rb[32]; int rl = encode_data_run(rb, data_cluster, data_cc);
            rb[rl] = 0; rl++;
            uint32_t as = (sizeof(attr_nonresident_t) + rl + 7) & ~7;
            attr_nonresident_t* nr = (attr_nonresident_t*)(rec + pos);
            nr->header.type = NTFS_ATTR_DATA; nr->header.length = as;
            nr->header.non_resident = 1; nr->header.attr_id = aid++;
            nr->end_vcn = data_cc ? data_cc - 1 : 0;
            nr->run_offset = sizeof(attr_nonresident_t);
            nr->alloc_size = (uint64_t)data_cc * fs_info.cluster_size;
            nr->real_size = data_size; nr->init_size = data_size;
            memcpy(rec + pos + sizeof(attr_nonresident_t), rb, rl);
            pos += as;
        }
    }

    *(uint32_t*)(rec + pos) = NTFS_ATTR_END; pos += 8;
    h->used_size = pos; h->next_attr_id = aid;
    return pos;
}

/* ================================================================
 * Index entry insert / remove
 * ================================================================ */

static uint32_t build_index_entry(uint8_t* out, uint64_t file_ref, uint64_t parent_ref,
                                    const char* name, uint32_t fsize, bool is_dir) {
    int nl = strlen(name);
    uint32_t fns = sizeof(filename_attr_t) + nl * 2;
    uint32_t es = (sizeof(index_entry_t) + fns + 7) & ~7;
    memset(out, 0, es);
    index_entry_t* ie = (index_entry_t*)out;
    ie->mft_ref = file_ref | ((uint64_t)1 << 48);
    ie->entry_length = (uint16_t)es; ie->content_length = (uint16_t)fns;
    filename_attr_t* fn = (filename_attr_t*)(out + sizeof(index_entry_t));
    fn->parent_ref = parent_ref | ((uint64_t)1 << 48);
    fn->real_size = is_dir ? 0 : fsize; fn->alloc_size = fn->real_size;
    fn->flags = is_dir ? NTFS_FILE_ATTR_DIRECTORY : NTFS_FILE_ATTR_ARCHIVE;
    fn->name_length = (uint8_t)nl; fn->name_type = 3;
    ascii_to_utf16le(name, (uint16_t*)((uint8_t*)fn + sizeof(filename_attr_t)), nl);
    return es;
}

static bool insert_index_entry(uint64_t dir_ref, uint8_t* new_ie, uint32_t new_sz) {
    uint8_t* dr = kmalloc(fs_info.mft_record_size); if (!dr) return false;
    if (!read_mft_record(dir_ref)) { kfree(dr); return false; }
    memcpy(dr, mft_buf, fs_info.mft_record_size);

    mft_record_header_t* h = (mft_record_header_t*)dr;
    attr_header_t* ira = find_attribute(dr, NTFS_ATTR_INDEX_ROOT, NULL);
    if (!ira) { kfree(dr); return false; }
    attr_resident_t* irr = (attr_resident_t*)ira;
    index_root_t* ir = (index_root_t*)((uint8_t*)ira + irr->value_offset);
    uint8_t* eb = (uint8_t*)ir + 0x10 + ir->entries_offset;

    /* Find LAST entry */
    uint32_t loff = 0, off = 0;
    while (off + sizeof(index_entry_t) <= ir->total_entries_size) {
        index_entry_t* ie = (index_entry_t*)(eb + off);
        if (ie->entry_length == 0) break;
        if (ie->flags & INDEX_ENTRY_LAST) { loff = off; break; }
        off += ie->entry_length;
    }

    if (h->used_size + new_sz > h->alloc_size - 8) { kfree(dr); return false; }

    /* Shift tail forward */
    uint32_t ts = (uint32_t)(eb + loff - dr);
    uint32_t tl = h->used_size - ts;
    for (int32_t i = (int32_t)tl - 1; i >= 0; i--)
        dr[ts + new_sz + i] = dr[ts + i];
    memcpy(dr + ts, new_ie, new_sz);

    ir->total_entries_size += new_sz; ir->alloc_entries_size += new_sz;
    irr->value_length += new_sz; ira->length += new_sz;
    h->used_size += new_sz;

    bool ok = write_mft_record(dir_ref, dr);
    kfree(dr); return ok;
}

static bool remove_index_entry(uint64_t dir_ref, const char* fname) {
    uint8_t* dr = kmalloc(fs_info.mft_record_size); if (!dr) return false;
    if (!read_mft_record(dir_ref)) { kfree(dr); return false; }
    memcpy(dr, mft_buf, fs_info.mft_record_size);

    mft_record_header_t* h = (mft_record_header_t*)dr;
    attr_header_t* ira = find_attribute(dr, NTFS_ATTR_INDEX_ROOT, NULL);
    if (!ira) { kfree(dr); return false; }
    attr_resident_t* irr = (attr_resident_t*)ira;
    index_root_t* ir = (index_root_t*)((uint8_t*)ira + irr->value_offset);
    uint8_t* eb = (uint8_t*)ir + 0x10 + ir->entries_offset;

    uint32_t off = 0; bool found = false; uint32_t fo = 0, fsz = 0;
    while (off + sizeof(index_entry_t) <= ir->total_entries_size) {
        index_entry_t* ie = (index_entry_t*)(eb + off);
        if (ie->entry_length == 0 || (ie->flags & INDEX_ENTRY_LAST)) break;
        if (ie->content_length >= sizeof(filename_attr_t)) {
            filename_attr_t* fn = (filename_attr_t*)(eb + off + sizeof(index_entry_t));
            char nm[NTFS_MAX_NAME];
            utf16le_to_ascii((uint16_t*)((uint8_t*)fn + sizeof(filename_attr_t)), fn->name_length, nm, NTFS_MAX_NAME);
            if (strcaseeq(nm, fname)) { found = true; fo = off; fsz = ie->entry_length; break; }
        }
        off += ie->entry_length;
    }
    if (!found) { kfree(dr); return false; }

    uint32_t ea = (uint32_t)(eb + fo - dr);
    uint32_t ta = ea + fsz, tl = h->used_size - ta;
    memcpy(dr + ea, dr + ta, tl); memset(dr + ea + tl, 0, fsz);
    ir->total_entries_size -= fsz; ir->alloc_entries_size -= fsz;
    irr->value_length -= fsz; ira->length -= fsz;
    h->used_size -= fsz;

    bool ok = write_mft_record(dir_ref, dr);
    kfree(dr); return ok;
}

/* ================================================================
 * Public API
 * ================================================================ */

bool ntfs_mount(void) {
    if (!ata_available()) return false;
    memset(&fs_info, 0, sizeof(fs_info));
    cluster_bitmap = NULL; mft_bitmap = NULL;

    if (!ata_read_sector(0, sector_buf)) return false;
    ntfs_boot_sector_t* bs = (ntfs_boot_sector_t*)sector_buf;
    if (memcmp(bs->oem_id, "NTFS    ", 8) != 0) return false;

    fs_info.bytes_per_sector = bs->bytes_per_sector;
    fs_info.sectors_per_cluster = bs->sectors_per_cluster;
    fs_info.cluster_size = (uint32_t)bs->bytes_per_sector * bs->sectors_per_cluster;
    fs_info.mft_cluster = bs->mft_lcn; fs_info.mft_mirror_cluster = bs->mft_mirr_lcn;
    fs_info.total_sectors = bs->total_sectors; fs_info.volume_serial = bs->volume_serial;

    if (bs->clusters_per_mft > 0) fs_info.mft_record_size = (uint32_t)bs->clusters_per_mft * fs_info.cluster_size;
    else fs_info.mft_record_size = 1U << (uint32_t)(-(int32_t)bs->clusters_per_mft);
    if (bs->clusters_per_index > 0) fs_info.index_record_size = (uint32_t)bs->clusters_per_index * fs_info.cluster_size;
    else fs_info.index_record_size = 1U << (uint32_t)(-(int32_t)bs->clusters_per_index);

    if (fs_info.bytes_per_sector != 512 || fs_info.cluster_size == 0 || fs_info.cluster_size > 65536) return false;
    if (fs_info.mft_record_size == 0 || fs_info.mft_record_size > 4096 || fs_info.mft_cluster == 0) return false;

    total_data_clusters = (uint32_t)div64_32(fs_info.total_sectors, fs_info.sectors_per_cluster);

    /* Volume label */
    strcpy(fs_info.volume_label, "NTFS");
    if (read_mft_record(MFT_RECORD_VOLUME)) {
        attr_header_t* vn = find_attribute(mft_buf, NTFS_ATTR_VOLUME_NAME, NULL);
        if (vn) { uint32_t vl; uint16_t* vd = (uint16_t*)get_resident_data(vn, &vl);
                  if (vd && vl > 0) utf16le_to_ascii(vd, vl / 2, fs_info.volume_label, sizeof(fs_info.volume_label)); }
    }

    /* MFT record count + MFT bitmap */
    fs_info.total_mft_records = 0;
    if (read_mft_record(MFT_RECORD_MFT)) {
        int64_t ms = get_data_size(mft_buf);
        if (ms > 0) fs_info.total_mft_records = (uint32_t)div64_32((uint64_t)ms, fs_info.mft_record_size);
        attr_header_t* mba = find_attribute(mft_buf, NTFS_ATTR_BITMAP, NULL);
        if (mba) {
            if (!mba->non_resident) {
                uint32_t bl; uint8_t* bd = (uint8_t*)get_resident_data(mba, &bl);
                if (bd && bl > 0) { mft_bitmap = kmalloc(bl);
                    if (mft_bitmap) { memcpy(mft_bitmap, bd, bl); mft_bitmap_bytes = bl; mft_bmp_resident = true; } }
            } else {
                attr_nonresident_t* nr = (attr_nonresident_t*)mba;
                uint32_t bsz = (uint32_t)nr->real_size;
                if (bsz > 0 && bsz <= 1024 * 1024 && parse_data_runs(mba, &mft_bmp_runs)) {
                    mft_bitmap = load_bitmap(&mft_bmp_runs, bsz);
                    if (mft_bitmap) { mft_bitmap_bytes = bsz; mft_bmp_resident = false; } }
            }
        }
    }

    /* Cluster bitmap ($Bitmap, MFT record 6) */
    if (read_mft_record(MFT_RECORD_BITMAP)) {
        attr_header_t* ba = find_attribute(mft_buf, NTFS_ATTR_DATA, NULL);
        if (ba && ba->non_resident) {
            attr_nonresident_t* nr = (attr_nonresident_t*)ba;
            uint32_t bsz = (uint32_t)nr->real_size;
            if (bsz > 0 && bsz <= 1024 * 1024 && parse_data_runs(ba, &cluster_bmp_runs)) {
                cluster_bitmap = load_bitmap(&cluster_bmp_runs, bsz);
                if (cluster_bitmap) cluster_bitmap_bytes = bsz;
            }
        }
    }

    fs_info.mounted = true;
    return true;
}

bool ntfs_mount_drive(int drv) {
    ntfs_drv = drv;
    return ntfs_mount();
}

int ntfs_get_drive_idx(void) { return ntfs_drv; }

void ntfs_unmount(void) {
    if (cluster_bitmap) { kfree(cluster_bitmap); cluster_bitmap = NULL; }
    if (mft_bitmap) { kfree(mft_bitmap); mft_bitmap = NULL; }
    cluster_bitmap_bytes = 0; mft_bitmap_bytes = 0;
    fs_info.mounted = false; memset(&fs_info, 0, sizeof(fs_info));
}

bool ntfs_is_mounted(void) { return fs_info.mounted; }
ntfs_info_t* ntfs_get_info(void) { return &fs_info; }

int ntfs_list_dir(const char* path, ntfs_dirent_t* entries, int max) {
    if (!fs_info.mounted) return -1;
    uint64_t dr = resolve_path(path);
    if (dr == (uint64_t)-1 || !read_mft_record(dr)) return -1;
    mft_record_header_t* h = (mft_record_header_t*)mft_buf;
    if (!(h->flags & MFT_RECORD_IN_USE) || !(h->flags & MFT_RECORD_IS_DIR)) return -1;
    uint8_t* dc = kmalloc(fs_info.mft_record_size); if (!dc) return -1;
    memcpy(dc, mft_buf, fs_info.mft_record_size);
    dir_list_ctx_t ctx = { entries, max, 0 };
    walk_directory(dc, dir_list_callback, &ctx);
    kfree(dc); return ctx.count;
}

int32_t ntfs_read_file(const char* path, void* buf, uint32_t max_size) {
    if (!fs_info.mounted) return -1;
    uint64_t fr = resolve_path(path);
    if (fr == (uint64_t)-1 || !read_mft_record(fr)) return -1;
    mft_record_header_t* h = (mft_record_header_t*)mft_buf;
    if (!(h->flags & MFT_RECORD_IN_USE) || (h->flags & MFT_RECORD_IS_DIR)) return -1;
    attr_header_t* da = find_attribute(mft_buf, NTFS_ATTR_DATA, NULL);
    while (da && da->name_length > 0) da = find_attribute(mft_buf, NTFS_ATTR_DATA, da);
    if (!da) return -1;
    if (da->non_resident) {
        run_list_t rl; if (!parse_data_runs(da, &rl)) return -1;
        return read_data_runs(&rl, buf, max_size, ((attr_nonresident_t*)da)->real_size);
    } else { uint32_t vl; void* d = get_resident_data(da, &vl); if (!d) return -1;
             uint32_t tc = (max_size < vl) ? max_size : vl; memcpy(buf, d, tc); return (int32_t)tc; }
}

int32_t ntfs_file_size(const char* path) {
    if (!fs_info.mounted) return -1;
    uint64_t fr = resolve_path(path);
    if (fr == (uint64_t)-1 || !read_mft_record(fr)) return -1;
    if (!(((mft_record_header_t*)mft_buf)->flags & MFT_RECORD_IN_USE)) return -1;
    int64_t s = get_data_size(mft_buf);
    return (s > 0x7FFFFFFF) ? 0x7FFFFFFF : (int32_t)s;
}

bool ntfs_is_dir(const char* path) {
    if (!fs_info.mounted) return false;
    uint64_t r = resolve_path(path);
    if (r == (uint64_t)-1 || !read_mft_record(r)) return false;
    mft_record_header_t* h = (mft_record_header_t*)mft_buf;
    return (h->flags & MFT_RECORD_IN_USE) && (h->flags & MFT_RECORD_IS_DIR);
}

void ntfs_print_info(void) {
    if (!fs_info.mounted) { kprintf("  NTFS: not mounted\n"); return; }
    kprintf("  NTFS Filesystem:\n");
    kprintf("    Label:       %s\n", fs_info.volume_label);
    kprintf("    Cluster:     %u bytes (%u sectors)\n", fs_info.cluster_size, fs_info.sectors_per_cluster);
    kprintf("    MFT record:  %u bytes\n", fs_info.mft_record_size);
    kprintf("    MFT cluster: %u\n", (uint32_t)fs_info.mft_cluster);
    kprintf("    MFT records: ~%u\n", fs_info.total_mft_records);
    kprintf("    Capacity:    %u MB\n", (uint32_t)((fs_info.total_sectors * fs_info.bytes_per_sector) >> 20));
    kprintf("    Write:       %s\n", (cluster_bitmap && mft_bitmap) ? "enabled" : "read-only (bitmaps not cached)");
}

/* ================================================================
 * Write operations
 * ================================================================ */

int32_t ntfs_write_file(const char* path, const void* data, uint32_t size) {
    if (!fs_info.mounted) return -1;
    if (!cluster_bitmap || !mft_bitmap) { kprintf("  NTFS: write not available\n"); return -1; }

    char dp[NTFS_MAX_PATH], fn[NTFS_MAX_NAME];
    split_path(path, dp, fn);
    if (!fn[0]) return -1;

    uint64_t parent = resolve_path(dp);
    if (parent == (uint64_t)-1) { kprintf("  NTFS: dir not found: %s\n", dp); return -1; }

    /* Overwrite: delete existing first */
    if (resolve_path(path) != (uint64_t)-1) ntfs_delete_file(path);

    uint64_t dc = 0; uint32_t cc = 0;
    uint32_t max_res = fs_info.mft_record_size - 0x38 - 200 - 24;
    if (size > max_res) {
        cc = (size + fs_info.cluster_size - 1) / fs_info.cluster_size;
        dc = alloc_clusters(cc);
        if (!dc) { kprintf("  NTFS: no space (%u clusters)\n", cc); return -1; }
    }

    uint32_t mn = alloc_mft_record();
    if (!mn) { if (dc) free_clusters(dc, cc); kprintf("  NTFS: no free MFT records\n"); return -1; }

    /* Write data to clusters */
    if (dc && data && size) {
        run_list_t rl; rl.count = 1; rl.total_clusters = cc;
        rl.runs[0].vcn = 0; rl.runs[0].lcn = dc; rl.runs[0].length = cc;
        if (write_data_runs(&rl, data, size) < 0) {
            free_clusters(dc, cc); free_mft_record(mn); return -1;
        }
    }

    /* Build and write MFT record */
    uint8_t* rec = kcalloc(1, fs_info.mft_record_size);
    if (!rec) {
        if (dc) free_clusters(dc, cc);
        free_mft_record(mn);
        return -1;
    }
    if (!build_file_record(rec, mn, parent, fn, false, data, size, dc, cc) ||
        !write_mft_record(mn, rec)) {
        kfree(rec);
        if (dc) free_clusters(dc, cc);
        free_mft_record(mn);
        return -1;
    }
    kfree(rec);

    /* Add to parent directory */
    uint8_t ieb[512];
    uint32_t ies = build_index_entry(ieb, mn, parent, fn, size, false);
    if (!insert_index_entry(parent, ieb, ies)) {
        if (dc) free_clusters(dc, cc);
        free_mft_record(mn);
        kprintf("  NTFS: dir entry failed (dir full?)\n");
        return -1;
    }
    return (int32_t)size;
}

int32_t ntfs_delete_file(const char* path) {
    if (!fs_info.mounted || !cluster_bitmap || !mft_bitmap) return -1;
    uint64_t fr = resolve_path(path);
    if (fr == (uint64_t)-1 || !read_mft_record(fr)) return -1;
    mft_record_header_t* h = (mft_record_header_t*)mft_buf;
    if (!(h->flags & MFT_RECORD_IN_USE) || (h->flags & MFT_RECORD_IS_DIR)) return -1;

    /* Free data clusters */
    attr_header_t* da = find_attribute(mft_buf, NTFS_ATTR_DATA, NULL);
    while (da && da->name_length > 0) da = find_attribute(mft_buf, NTFS_ATTR_DATA, da);
    if (da && da->non_resident) {
        run_list_t rl; if (parse_data_runs(da, &rl))
            for (int i = 0; i < rl.count; i++) free_clusters(rl.runs[i].lcn, (uint32_t)rl.runs[i].length);
    }

    /* Get parent + name for index removal */
    char fname[NTFS_MAX_NAME] = {0}; uint64_t pref = 0;
    attr_header_t* fa = find_attribute(mft_buf, NTFS_ATTR_FILE_NAME, NULL);
    if (fa) { uint32_t vl; filename_attr_t* fn = (filename_attr_t*)get_resident_data(fa, &vl);
              if (fn) { pref = fn->parent_ref & 0x0000FFFFFFFFFFFFULL;
                        utf16le_to_ascii((uint16_t*)((uint8_t*)fn + sizeof(filename_attr_t)),
                                         fn->name_length, fname, NTFS_MAX_NAME); } }

    h->flags &= ~MFT_RECORD_IN_USE;
    write_mft_record(fr, mft_buf);
    free_mft_record((uint32_t)fr);
    if (fname[0] && pref) remove_index_entry(pref, fname);
    return 0;
}

int32_t ntfs_mkdir(const char* path) {
    if (!fs_info.mounted || !cluster_bitmap || !mft_bitmap) return -1;
    char dp[NTFS_MAX_PATH], dn[NTFS_MAX_NAME];
    split_path(path, dp, dn);
    if (!dn[0]) return -1;
    uint64_t parent = resolve_path(dp);
    if (parent == (uint64_t)-1 || resolve_path(path) != (uint64_t)-1) return -1;

    uint32_t mn = alloc_mft_record();
    if (!mn) return -1;
    uint8_t* rec = kcalloc(1, fs_info.mft_record_size);
    if (!rec) { free_mft_record(mn); return -1; }
    if (!build_file_record(rec, mn, parent, dn, true, NULL, 0, 0, 0) ||
        !write_mft_record(mn, rec)) { kfree(rec); free_mft_record(mn); return -1; }
    kfree(rec);

    uint8_t ieb[512];
    uint32_t ies = build_index_entry(ieb, mn, parent, dn, 0, true);
    if (!insert_index_entry(parent, ieb, ies)) { free_mft_record(mn); return -1; }
    return 0;
}
