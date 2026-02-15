#ifndef PROCFS_H
#define PROCFS_H

#include "types.h"
#include "ramfs.h"

#define PROCFS_MAX_CONTENT 4096

/* Initialize /proc virtual filesystem */
void procfs_init(void);

/* Check if a path is a /proc virtual file */
bool procfs_is_virtual(const char* path);

/* Read a /proc virtual file into buf. Returns bytes written or -1 */
int32_t procfs_read(const char* path, void* buf, uint32_t max);

/* List /proc directory entries */
void procfs_list(void);

/* Check if path is /proc directory */
bool procfs_is_dir(const char* path);

/* Stat a /proc entry: returns 0 on success */
int32_t procfs_stat(const char* path, ramfs_type_t* type, uint32_t* size);

/* Helper: kernel snprintf (supports %s %d %u %x %%) */
int ksnprintf(char* buf, int max, const char* fmt, ...);

#endif
