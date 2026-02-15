#ifndef RAMFS_H
#define RAMFS_H

#include "types.h"

#define RAMFS_MAX_FILES   64
#define RAMFS_MAX_NAME    32
#define RAMFS_MAX_DATA    (50 * 1024 * 1024)   /* 50MB per file max */
#define RAMFS_MAX_PATH    128

typedef enum { RAMFS_FILE, RAMFS_DIR } ramfs_type_t;

typedef struct {
    bool         active;
    char         name[RAMFS_MAX_NAME];
    ramfs_type_t type;
    uint8_t*     data;          /* Heap-allocated file data */
    uint32_t     size;
    uint32_t     capacity;      /* Allocated size */
    int32_t      parent;        /* Index of parent directory (-1 for root) */
    uint32_t     created;       /* Tick when created */
    uint32_t     modified;      /* Tick when last modified */
} ramfs_node_t;

void     ramfs_init(void);
int32_t  ramfs_create(const char* path, ramfs_type_t type);
int32_t  ramfs_write(const char* path, const void* data, uint32_t size);
int32_t  ramfs_append(const char* path, const void* data, uint32_t size);
int32_t  ramfs_read(const char* path, void* buf, uint32_t max);
int32_t  ramfs_delete(const char* path);
int32_t  ramfs_find(const char* path);
void     ramfs_list(const char* dir_path);
void     ramfs_tree(const char* dir_path, int depth);
int32_t  ramfs_stat(const char* path, ramfs_type_t* type, uint32_t* size);
uint32_t ramfs_file_count(void);
uint32_t ramfs_total_size(void);
int32_t  ramfs_rename(const char* old_path, const char* new_path);
ramfs_node_t* ramfs_get_node(int32_t idx);
void     ramfs_get_path(int32_t idx, char* buf, uint32_t max);

/* Current working directory */
void        ramfs_set_cwd(const char* path);
const char* ramfs_get_cwd(void);
void        ramfs_resolve_path(const char* input, char* output);

#endif
