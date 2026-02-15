#ifndef USERLIB_H
#define USERLIB_H

/*
 * MicroKernel User-Space Library
 *
 * Self-contained header for server binaries that run in isolated
 * address spaces. NO kernel headers are included — everything the
 * server needs is defined here.
 *
 * Servers communicate with the kernel through INT 0x80 syscalls
 * and with each other through synchronous IPC messages.
 */

/* ---- Basic types ---- */
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed int         int32_t;
typedef unsigned int       size_t;

#define NULL ((void*)0)
#define true  1
#define false 0
typedef int bool;

/* ---- Port I/O (available to IOPL=3 servers) ---- */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ---- Syscall numbers ---- */

#define SYS_WRITE           1
#define SYS_READ            2
#define SYS_GETPID          5
#define SYS_EXIT            6
#define SYS_SLEEP           7
#define SYS_TIME            8

#define SYS_SEND            20
#define SYS_RECEIVE         21
#define SYS_SENDREC         22
#define SYS_REPLY           23
#define SYS_NOTIFY          24

#define SYS_REGISTER_SVC    30
#define SYS_LOOKUP_SVC      31
#define SYS_REGISTER_IRQ    33

#define SYS_KBD_GETCHAR     40
#define SYS_RAMFS_READ      41
#define SYS_RAMFS_WRITE     42
#define SYS_RAMFS_CREATE    43
#define SYS_RAMFS_DELETE    44
#define SYS_PROCFS_READ     45
#define SYS_FAT16_READ      46
#define SYS_FAT16_WRITE     47
#define SYS_FAT16_MOUNTED   48
#define SYS_NTFS_READ       49
#define SYS_NTFS_WRITE      50
#define SYS_NTFS_MOUNTED    51
#define SYS_ATA_READ        52
#define SYS_ATA_WRITE       53
#define SYS_ATA_INFO        54
#define SYS_NET_STATUS      55
#define SYS_NET_POLL        56
#define SYS_DEBUG_LOG       57

#define SYS_GUI_WIN_OPEN    60
#define SYS_GUI_PRESENT     61
#define SYS_GUI_WIN_CLOSE   62
#define SYS_GUI_GET_TICKS   63

/* ---- GUI framebuffer constants ---- */
#define ELF_GUI_FB_W    320
#define ELF_GUI_FB_H    200

/* ---- IPC message types ---- */

#define MSG_CONS_WRITE      10
#define MSG_CONS_READ       11
#define MSG_CONS_PUTCHAR    12
#define MSG_CONS_CLEAR      13
#define MSG_CONS_SETCOLOR   14
#define MSG_CONS_GETCHAR    15

#define MSG_VFS_READ        20
#define MSG_VFS_WRITE       21
#define MSG_VFS_CREATE      22
#define MSG_VFS_DELETE      23
#define MSG_VFS_LIST        24

#define MSG_DISK_READ       30
#define MSG_DISK_WRITE      31
#define MSG_DISK_INFO       32

#define MSG_NET_SEND        40
#define MSG_NET_RECV        41
#define MSG_NET_IFCONFIG    42
#define MSG_NET_PING        43

#define MSG_IRQ_NOTIFY      100
#define MSG_REPLY           101

#define PID_ANY             0xFFFFFFFF


#define SYS_VIRGL_SUBMIT    70

/* GPU 3D high-level API */
#define SYS_GPU3D_INIT      71
#define SYS_GPU3D_CLEAR     72
#define SYS_GPU3D_UPLOAD    73
#define SYS_GPU3D_MVP       74
#define SYS_GPU3D_DRAW      75
#define SYS_GPU3D_PRESENT   76

/* GPU3D primitive types (Gallium PIPE_PRIM_*) */
#define GPU3D_TRIANGLES      4
#define GPU3D_TRIANGLE_STRIP 5
#define GPU3D_TRIANGLE_FAN   6
#define GPU3D_LINES          1
#define GPU3D_LINE_STRIP     3
#define GPU3D_POINTS         0

/* GPU3D clear flags */
/* These MUST match Gallium pipe_clear_flags */
#define GPU3D_CLEAR_DEPTH   1   /* PIPE_CLEAR_DEPTH */
#define GPU3D_CLEAR_STENCIL 2   /* PIPE_CLEAR_STENCIL */
#define GPU3D_CLEAR_COLOR   4   /* PIPE_CLEAR_COLOR0 — NOT 1! */

// Under "Syscall wrappers"
int32_t sys_virgl_submit(void* cmd_buf, uint32_t size);

/* GPU 3D syscall wrappers */
int32_t  sys_gpu3d_init(uint32_t width, uint32_t height);
int32_t  sys_gpu3d_clear(uint32_t flags, uint32_t color_argb);
int32_t  sys_gpu3d_upload(const float* vertices, uint32_t num_floats);
int32_t  sys_gpu3d_set_mvp(const float* mvp16);
int32_t  sys_gpu3d_draw(uint32_t prim_mode, uint32_t start, uint32_t count);
int32_t  sys_gpu3d_present(void);

// Under "String/memory utilities"
char* utoa(uint32_t value, char* str, int base);

/* ---- IPC message structure (must match kernel's message_t) ---- */

typedef struct {
    uint32_t sender;
    uint32_t type;
    union {
        uint8_t raw[56];
        struct {
            uint32_t fd;
            uint32_t offset;
            uint32_t size;
            uint32_t buf;
            char     path[40];
        } io;
        struct {
            uint32_t irq;
            uint32_t ticks;
        } interrupt;
        struct {
            int32_t  status;
            uint32_t value;
            uint32_t size;
            uint32_t buf;
            uint8_t  data[40];
        } reply;
        struct {
            uint32_t len;
            uint8_t  color;
            char     data[47];
        } cons;
    };
} message_t;

/* ---- Service names ---- */
#define SVC_CONSOLE     "console"
#define SVC_VFS         "vfs"
#define SVC_DISK        "ata"
#define SVC_NET         "net"

/* ---- VGA constants (for console server) ---- */

/* VGA text buffer is mapped at this virtual address by the ELF loader */
#define USER_VGA_VADDR  0xB0000000
#define VGA_WIDTH       80
#define VGA_HEIGHT      25
#define VGA_BUFFER      ((volatile uint16_t*)USER_VGA_VADDR)

/* VGA I/O ports (require IOPL=3) */
#define VGA_CTRL_PORT   0x3D4
#define VGA_DATA_PORT   0x3D5

/* ---- Syscall wrappers ---- */

int32_t sys_send(uint32_t dest, message_t* msg);
int32_t sys_receive(uint32_t from, message_t* msg);
int32_t sys_sendrec(uint32_t dest, message_t* msg);
int32_t sys_reply(uint32_t dest, message_t* msg);
int32_t sys_notify(uint32_t dest, message_t* msg);
int32_t sys_register_service(const char* name);
uint32_t sys_lookup_service(const char* name);
int32_t sys_register_irq(uint32_t irq);
uint32_t sys_getpid(void);
void    sys_exit(int code);
void    sys_sleep(uint32_t ms);   

/* Server-support syscalls */
char    sys_kbd_getchar(void);
int32_t sys_ramfs_read(const char* path, char* buf, uint32_t max);
int32_t sys_ramfs_write(const char* path, const char* buf, uint32_t size);
int32_t sys_ramfs_create(const char* path, uint32_t type);
int32_t sys_ramfs_delete(const char* path);
int32_t sys_procfs_read(const char* path, char* buf, uint32_t max);
int32_t sys_fat16_read(const char* path, uint8_t* buf, uint32_t max);
int32_t sys_fat16_write(const char* path, const uint8_t* buf, uint32_t size);
int32_t sys_fat16_mounted(void);
int32_t sys_ntfs_read(const char* path, uint8_t* buf, uint32_t max);
int32_t sys_ntfs_write(const char* path, const uint8_t* buf, uint32_t size);
int32_t sys_ntfs_mounted(void);
int32_t sys_ata_read(uint32_t lba, uint8_t* buf, uint32_t count);
int32_t sys_ata_write(uint32_t lba, const uint8_t* buf, uint32_t count);
int32_t sys_ata_info(uint32_t drive);
int32_t sys_net_status(void);
int32_t sys_net_poll(void);
void    sys_debug_log(const char* msg);

/* GUI window syscalls */
uint32_t sys_gui_win_open(const char* title);  /* returns framebuffer address */
void     sys_gui_present(void);                /* signal frame ready + yield */
void     sys_gui_win_close(void);              /* close window */
uint32_t sys_gui_get_ticks(void);              /* get timer ticks */

/* ---- Math utilities for user-space rendering ---- */

static inline float u_fabs(float x) { return x < 0 ? -x : x; }

static inline float u_fmod(float x, float y) {
    return x - (int)(x / y) * y;
}

static inline float u_sin(float x) {
    /* Normalize to [-PI, PI] */
    const float PI = 3.14159265358979f;
    x = u_fmod(x + PI, 2 * PI) - PI;
    /* Bhaskara I approximation improved with cubic term */
    float x2 = x * x;
    return x * (PI*PI - 4*x2) / (PI*PI + x2);
}

static inline float u_cos(float x) {
    return u_sin(x + 1.5707963f);
}

/* ---- String/memory utilities ---- */

static inline size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static inline int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char*)a - *(unsigned char*)b;
}

static inline int strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

static inline char* strcpy(char* dst, const char* src) {
    char* d = dst;
    while ((*d++ = *src++));
    return dst;
}

static inline char* strncpy(char* dst, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

static inline void* memset(void* dst, int c, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

static inline void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dst;
}


/* Add these to your userlib.h for user-space programs */

/* Syscall wrapper for getting time */
static inline uint32_t sys_get_time(void) {
    uint32_t ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(19)  /* SYS_GET_TIME - use your actual syscall number */
    );
    return ret;
}

static inline uint32_t sys_get_time_ms(void) {
    uint32_t ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(20)  /* SYS_GET_TIME_MS - use your actual syscall number */
    );
    return ret;
}

uint32_t sys_get_ticks(void);



#endif
