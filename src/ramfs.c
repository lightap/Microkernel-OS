#include "ramfs.h"
#include "timer.h"
#include "vga.h"
#include "heap.h"
#include "procfs.h"
#include "fat16.h"
#include "ntfs.h"

static ramfs_node_t nodes[RAMFS_MAX_FILES];
static char cwd[RAMFS_MAX_PATH] = "/";

/* Determine if a resolved path targets /disk (drive 0) or /disk2 (drive 1).
 * Returns drive index (0 or 1) + 1, or 0 if not a disk path.
 * Sets *rel to the filesystem-relative path (e.g. "/" or "/subdir/file"). */
static int match_disk(const char* resolved, const char** rel) {
    /* Must check /disk2 before /disk since /disk is a prefix of /disk2 */
    if (strncmp(resolved, "/disk2", 6) == 0) {
        if (resolved[6] == '\0') { *rel = "/"; return 2; }
        if (resolved[6] == '/') { *rel = resolved + 6; return 2; }
    }
    if (strncmp(resolved, "/disk", 5) == 0) {
        if (resolved[5] == '\0') { *rel = "/"; return 1; }
        if (resolved[5] == '/') { *rel = resolved + 5; return 1; }
    }
    *rel = NULL; return 0;
}

/* Same but excludes mount point root (for create/delete operations). */
static int match_disk_content(const char* resolved, const char** rel) {
    if (strncmp(resolved, "/disk2/", 7) == 0 && resolved[7] != '\0') {
        *rel = resolved + 6; return 2;
    }
    if (strncmp(resolved, "/disk/", 6) == 0 && resolved[6] != '\0') {
        *rel = resolved + 5; return 1;
    }
    *rel = NULL; return 0;
}

/* Check if FAT16 is mounted on the given drive index (0 or 1) */
static bool is_fat16_on(int drv) { return fat16_is_mounted() && fat16_get_drive_idx() == drv; }
static bool is_ntfs_on(int drv) { return ntfs_is_mounted() && ntfs_get_drive_idx() == drv; }

void ramfs_init(void) {
    memset(nodes, 0, sizeof(nodes));
    nodes[0].active = true;
    strcpy(nodes[0].name, "/");
    nodes[0].type = RAMFS_DIR;
    nodes[0].parent = -1;
    nodes[0].data = NULL;
    nodes[0].capacity = 0;
    nodes[0].created = timer_get_ticks();
    nodes[0].modified = timer_get_ticks();
}

static int32_t parse_path(const char* path, char* filename) {
    char resolved[RAMFS_MAX_PATH];
    ramfs_resolve_path(path, resolved);
    const char* last_slash = resolved;
    for (const char* p = resolved; *p; p++)
        if (*p == '/') last_slash = p;
    if (last_slash == resolved && resolved[0] == '/') {
        strcpy(filename, last_slash + 1);
        return 0;
    }
    strcpy(filename, last_slash + 1);
    char parent_path[RAMFS_MAX_PATH];
    size_t plen = last_slash - resolved;
    if (plen == 0) plen = 1;
    strncpy(parent_path, resolved, plen);
    parent_path[plen] = '\0';
    return ramfs_find(parent_path);
}

void ramfs_resolve_path(const char* input, char* output) {
    if (input[0] == '/') {
        strcpy(output, input);
    } else {
        strcpy(output, cwd);
        if (output[strlen(output) - 1] != '/') strcat(output, "/");
        strcat(output, input);
    }
    size_t len = strlen(output);
    if (len > 1 && output[len - 1] == '/') output[len - 1] = '\0';
}

int32_t ramfs_find(const char* path) {
    char resolved[RAMFS_MAX_PATH];
    ramfs_resolve_path(path, resolved);
    if (strcmp(resolved, "/") == 0) return 0;
    char* p = resolved + 1;
    int32_t current = 0;
    while (*p) {
        char component[RAMFS_MAX_NAME];
        int ci = 0;
        while (*p && *p != '/' && ci < RAMFS_MAX_NAME - 1)
            component[ci++] = *p++;
        component[ci] = '\0';
        if (*p == '/') p++;
        if (strcmp(component, ".") == 0) continue;
        if (strcmp(component, "..") == 0) {
            if (nodes[current].parent >= 0)
                current = nodes[current].parent;
            continue;
        }
        bool found = false;
        for (int i = 0; i < RAMFS_MAX_FILES; i++) {
            if (nodes[i].active && nodes[i].parent == current &&
                strcmp(nodes[i].name, component) == 0) {
                current = i;
                found = true;
                break;
            }
        }
        if (!found) return -1;
    }
    return current;
}

int32_t ramfs_create(const char* path, ramfs_type_t type) {
    char resolved[RAMFS_MAX_PATH];
    ramfs_resolve_path(path, resolved);
    const char* dp;
    int dm = match_disk_content(resolved, &dp);
    if (dm && type == RAMFS_DIR) {
        int drv = dm - 1;
        if (is_fat16_on(drv)) return fat16_mkdir(dp);
        if (is_ntfs_on(drv))  return ntfs_mkdir(dp);
    }

    char filename[RAMFS_MAX_NAME];
    int32_t parent = parse_path(path, filename);
    if (parent < 0 || strlen(filename) == 0) return -1;
    if (nodes[parent].type != RAMFS_DIR) return -2;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (nodes[i].active && nodes[i].parent == parent &&
            strcmp(nodes[i].name, filename) == 0)
            return -3;
    }
    for (int i = 1; i < RAMFS_MAX_FILES; i++) {
        if (!nodes[i].active) {
            nodes[i].active = true;
            strcpy(nodes[i].name, filename);
            nodes[i].type = type;
            nodes[i].parent = parent;
            nodes[i].size = 0;
            nodes[i].data = NULL;
            nodes[i].capacity = 0;
            nodes[i].created = timer_get_ticks();
            nodes[i].modified = timer_get_ticks();
            return i;
        }
    }
    return -4;
}

int32_t ramfs_write(const char* path, const void* data, uint32_t size) {
    char resolved[RAMFS_MAX_PATH];
    ramfs_resolve_path(path, resolved);
    const char* dp;
    int dm = match_disk_content(resolved, &dp);
    if (dm) {
        int drv = dm - 1;
        if (is_fat16_on(drv)) return fat16_write_file(dp, data, size);
        if (is_ntfs_on(drv))  return ntfs_write_file(dp, data, size);
    }

    int32_t idx = ramfs_find(path);
    if (idx < 0) {
        idx = ramfs_create(path, RAMFS_FILE);
        if (idx < 0) return idx;
    }
    if (nodes[idx].type != RAMFS_FILE) return -1;
    if (size > RAMFS_MAX_DATA) size = RAMFS_MAX_DATA;

    if (size > nodes[idx].capacity) {
        if (nodes[idx].data) kfree(nodes[idx].data);
        nodes[idx].data = kmalloc(size);
        if (!nodes[idx].data) {
            nodes[idx].capacity = 0;
            nodes[idx].size = 0;
            return -5;
        }
        nodes[idx].capacity = size;
    }

    memcpy(nodes[idx].data, data, size);
    nodes[idx].size = size;
    nodes[idx].modified = timer_get_ticks();
    return size;
}

int32_t ramfs_append(const char* path, const void* data, uint32_t size) {
    int32_t idx = ramfs_find(path);
    if (idx < 0) return ramfs_write(path, data, size);
    if (nodes[idx].type != RAMFS_FILE) return -1;

    uint32_t new_size = nodes[idx].size + size;
    if (new_size > RAMFS_MAX_DATA) new_size = RAMFS_MAX_DATA;
    uint32_t to_add = new_size - nodes[idx].size;

    if (new_size > nodes[idx].capacity) {
        uint8_t* new_data = kmalloc(new_size);
        if (!new_data) return -5;
        if (nodes[idx].data && nodes[idx].size > 0)
            memcpy(new_data, nodes[idx].data, nodes[idx].size);
        if (nodes[idx].data) kfree(nodes[idx].data);
        nodes[idx].data = new_data;
        nodes[idx].capacity = new_size;
    }

    memcpy(nodes[idx].data + nodes[idx].size, data, to_add);
    nodes[idx].size = new_size;
    nodes[idx].modified = timer_get_ticks();
    return to_add;
}

int32_t ramfs_read(const char* path, void* buf, uint32_t max) {
    /* Intercept /proc virtual files */
    char resolved[RAMFS_MAX_PATH];
    ramfs_resolve_path(path, resolved);
    if (procfs_is_virtual(resolved)) {
        return procfs_read(resolved, buf, max);
    }

    /* Intercept disk paths */
    const char* dp;
    int dm = match_disk(resolved, &dp);
    if (dm) {
        int drv = dm - 1;
        if (is_fat16_on(drv)) return fat16_read_file(dp, buf, max);
        if (is_ntfs_on(drv))  return ntfs_read_file(dp, buf, max);
    }

    int32_t idx = ramfs_find(path);
    if (idx < 0 || nodes[idx].type != RAMFS_FILE) return -1;
    if (!nodes[idx].data) return 0;
    uint32_t to_read = (nodes[idx].size < max) ? nodes[idx].size : max;
    memcpy(buf, nodes[idx].data, to_read);
    return to_read;
}

int32_t ramfs_delete(const char* path) {
    char resolved[RAMFS_MAX_PATH];
    ramfs_resolve_path(path, resolved);
    const char* dp;
    int dm = match_disk_content(resolved, &dp);
    if (dm) {
        int drv = dm - 1;
        if (is_fat16_on(drv)) return fat16_delete_file(dp);
        if (is_ntfs_on(drv))  return ntfs_delete_file(dp);
    }

    int32_t idx = ramfs_find(path);
    if (idx <= 0) return -1;
    if (nodes[idx].type == RAMFS_DIR) {
        for (int i = 0; i < RAMFS_MAX_FILES; i++)
            if (nodes[i].active && nodes[i].parent == idx)
                return -2;
    }
    if (nodes[idx].data) kfree(nodes[idx].data);
    nodes[idx].data = NULL;
    nodes[idx].capacity = 0;
    nodes[idx].active = false;
    return 0;
}

int32_t ramfs_stat(const char* path, ramfs_type_t* type, uint32_t* size) {
    /* Intercept /proc virtual files */
    char resolved[RAMFS_MAX_PATH];
    ramfs_resolve_path(path, resolved);
    if (procfs_is_virtual(resolved) && strlen(resolved) > 5) {
        return procfs_stat(resolved, type, size);
    }

    /* Intercept disk paths */
    const char* dp;
    int dm = match_disk(resolved, &dp);
    if (dm) {
        int drv = dm - 1;
        if (is_fat16_on(drv)) {
            if (strcmp(dp, "/") == 0 || fat16_is_dir(dp)) {
                if (type) *type = RAMFS_DIR;
                if (size) *size = 0;
                return 0;
            }
            int32_t fsize = fat16_file_size(dp);
            if (fsize >= 0) {
                if (type) *type = RAMFS_FILE;
                if (size) *size = (uint32_t)fsize;
                return 0;
            }
            return -1;
        }
        if (is_ntfs_on(drv)) {
            if (strcmp(dp, "/") == 0 || ntfs_is_dir(dp)) {
                if (type) *type = RAMFS_DIR;
                if (size) *size = 0;
                return 0;
            }
            int32_t fsize = ntfs_file_size(dp);
            if (fsize >= 0) {
                if (type) *type = RAMFS_FILE;
                if (size) *size = (uint32_t)fsize;
                return 0;
            }
            return -1;
        }
    }

    int32_t idx = ramfs_find(path);
    if (idx < 0) return -1;
    if (type) *type = nodes[idx].type;
    if (size) *size = nodes[idx].size;
    return 0;
}

void ramfs_list(const char* dir_path) {
    /* Intercept /proc listing */
    char resolved[RAMFS_MAX_PATH];
    ramfs_resolve_path(dir_path, resolved);
    if (strcmp(resolved, "/proc") == 0) {
        procfs_list();
        return;
    }

    /* Intercept disk listing */
    const char* dp;
    int dm = match_disk(resolved, &dp);
    if (dm) {
        int drv = dm - 1;
        if (is_fat16_on(drv)) {
            fat16_dirent_t entries[64];
            int count = fat16_list_dir(dp, entries, 64);
            if (count < 0) { kprintf("  Cannot read disk directory\n"); return; }
            if (count == 0) { kprintf("  (empty)\n"); return; }
            for (int i = 0; i < count; i++) {
                if (entries[i].is_dir) {
                    terminal_print_colored(entries[i].name, 0x09);
                    terminal_print_colored("/", 0x09);
                } else {
                    kprintf("%s", entries[i].name);
                }
                if (!entries[i].is_dir) {
                    if (entries[i].size >= 1024*1024)
                        kprintf("  %uM", entries[i].size / (1024*1024));
                    else if (entries[i].size >= 1024)
                        kprintf("  %uK", entries[i].size / 1024);
                    else
                        kprintf("  %uB", entries[i].size);
                }
                kprintf("\n");
            }
            return;
        }
        if (is_ntfs_on(drv)) {
            ntfs_dirent_t entries[64];
            int count = ntfs_list_dir(dp, entries, 64);
            if (count < 0) { kprintf("  Cannot read disk directory\n"); return; }
            if (count == 0) { kprintf("  (empty)\n"); return; }
            for (int i = 0; i < count; i++) {
                if (entries[i].is_dir) {
                    terminal_print_colored(entries[i].name, 0x09);
                    terminal_print_colored("/", 0x09);
                } else {
                    kprintf("%s", entries[i].name);
                }
                if (!entries[i].is_dir) {
                    if (entries[i].size >= 1024*1024)
                        kprintf("  %uM", (uint32_t)(entries[i].size / (1024*1024)));
                    else if (entries[i].size >= 1024)
                        kprintf("  %uK", (uint32_t)(entries[i].size / 1024));
                    else
                        kprintf("  %uB", (uint32_t)entries[i].size);
                }
                kprintf("\n");
            }
            return;
        }
    }

    int32_t dir = ramfs_find(dir_path);
    if (dir < 0) { kprintf("  Directory not found\n"); return; }
    if (nodes[dir].type != RAMFS_DIR) { kprintf("  Not a directory\n"); return; }
    bool any = false;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (nodes[i].active && nodes[i].parent == dir) {
            any = true;
            if (nodes[i].type == RAMFS_DIR) {
                terminal_print_colored(nodes[i].name, 0x09);
                terminal_print_colored("/", 0x09);
            } else {
                kprintf("%s", nodes[i].name);
            }
            if (nodes[i].size >= 1024 * 1024)
                kprintf("  (%u MB)\n", nodes[i].size / (1024 * 1024));
            else if (nodes[i].size >= 1024)
                kprintf("  (%u KB)\n", nodes[i].size / 1024);
            else
                kprintf("  (%u bytes)\n", nodes[i].size);
        }
    }
    if (!any) kprintf("  (empty)\n");
}

void ramfs_tree(const char* dir_path, int depth) {
    int32_t dir = ramfs_find(dir_path);
    if (dir < 0) return;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (nodes[i].active && nodes[i].parent == dir) {
            for (int d = 0; d < depth; d++) kprintf("  ");
            if (nodes[i].type == RAMFS_DIR) {
                terminal_print_colored(nodes[i].name, 0x09);
                terminal_print_colored("/\n", 0x09);
                char subpath[RAMFS_MAX_PATH] = "";
                if (strcmp(dir_path, "/") == 0) {
                    subpath[0] = '/';
                    strcpy(subpath + 1, nodes[i].name);
                } else {
                    strcpy(subpath, dir_path);
                    strcat(subpath, "/");
                    strcat(subpath, nodes[i].name);
                }
                ramfs_tree(subpath, depth + 1);
            } else {
                kprintf("%s (%u B)\n", nodes[i].name, nodes[i].size);
            }
        }
    }
}

void ramfs_set_cwd(const char* path) {
    char resolved[RAMFS_MAX_PATH];
    ramfs_resolve_path(path, resolved);
    int32_t idx = ramfs_find(resolved);
    if (idx >= 0 && nodes[idx].type == RAMFS_DIR)
        strcpy(cwd, resolved);
}

const char* ramfs_get_cwd(void) { return cwd; }

uint32_t ramfs_file_count(void) {
    uint32_t c = 0;
    for (int i = 0; i < RAMFS_MAX_FILES; i++)
        if (nodes[i].active) c++;
    return c;
}

uint32_t ramfs_total_size(void) {
    uint32_t s = 0;
    for (int i = 0; i < RAMFS_MAX_FILES; i++)
        if (nodes[i].active && nodes[i].type == RAMFS_FILE)
            s += nodes[i].size;
    return s;
}

int32_t ramfs_rename(const char* old_path, const char* new_path) {
    int32_t src = ramfs_find(old_path);
    if (src <= 0) return -1;
    char new_name[RAMFS_MAX_NAME];
    int32_t new_parent = parse_path(new_path, new_name);
    if (new_parent < 0 || strlen(new_name) == 0) return -2;
    if (nodes[new_parent].type != RAMFS_DIR) return -3;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (nodes[i].active && nodes[i].parent == new_parent &&
            strcmp(nodes[i].name, new_name) == 0)
            return -4;
    }
    strncpy(nodes[src].name, new_name, RAMFS_MAX_NAME - 1);
    nodes[src].name[RAMFS_MAX_NAME - 1] = '\0';
    nodes[src].parent = new_parent;
    nodes[src].modified = timer_get_ticks();
    return 0;
}

ramfs_node_t* ramfs_get_node(int32_t idx) {
    if (idx < 0 || idx >= RAMFS_MAX_FILES) return NULL;
    if (!nodes[idx].active) return NULL;
    return &nodes[idx];
}

void ramfs_get_path(int32_t idx, char* buf, uint32_t max) {
    if (idx < 0 || idx >= RAMFS_MAX_FILES || !nodes[idx].active) {
        buf[0] = '\0';
        return;
    }
    if (idx == 0) { strcpy(buf, "/"); return; }
    char parts[8][RAMFS_MAX_NAME];
    int depth = 0;
    int32_t cur = idx;
    while (cur > 0 && depth < 8) {
        strcpy(parts[depth++], nodes[cur].name);
        cur = nodes[cur].parent;
    }
    buf[0] = '\0';
    for (int i = depth - 1; i >= 0; i--) {
        if (strlen(buf) + strlen(parts[i]) + 2 < max) {
            strcat(buf, "/");
            strcat(buf, parts[i]);
        }
    }
    if (buf[0] == '\0') strcpy(buf, "/");
}
