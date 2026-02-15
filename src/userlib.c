#include "userlib.h"

/*
 * User-space syscall wrappers.
 * Each function triggers INT 0x80 with the syscall number in EAX
 * and arguments in EBX, ECX, EDX.
 */

/* ---- IPC syscalls ---- */

int32_t sys_send(uint32_t dest, message_t* msg) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_SEND), "b"(dest), "c"((uint32_t)msg)
        : "memory");
    return ret;
}

int32_t sys_receive(uint32_t from, message_t* msg) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_RECEIVE), "b"(from), "c"((uint32_t)msg)
        : "memory");
    return ret;
}

int32_t sys_sendrec(uint32_t dest, message_t* msg) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_SENDREC), "b"(dest), "c"((uint32_t)msg)
        : "memory");
    return ret;
}

int32_t sys_reply(uint32_t dest, message_t* msg) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_REPLY), "b"(dest), "c"((uint32_t)msg)
        : "memory");
    return ret;
}

int32_t sys_notify(uint32_t dest, message_t* msg) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_NOTIFY), "b"(dest), "c"((uint32_t)msg)
        : "memory");
    return ret;
}

int32_t sys_register_service(const char* name) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_REGISTER_SVC), "b"((uint32_t)name)
        : "memory");
    return ret;
}

uint32_t sys_lookup_service(const char* name) {
    uint32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_LOOKUP_SVC), "b"((uint32_t)name)
        : "memory");
    return ret;
}

int32_t sys_register_irq(uint32_t irq) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_REGISTER_IRQ), "b"(irq)
        : "memory");
    return ret;
}

uint32_t sys_getpid(void) {
    uint32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_GETPID));
    return ret;
}

void sys_exit(int code) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_EXIT), "b"(code));
}

/* ---- Server-support syscalls ---- */

char sys_kbd_getchar(void) {
    int32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_KBD_GETCHAR));
    return (char)ret;
}

int32_t sys_ramfs_read(const char* path, char* buf, uint32_t max) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_RAMFS_READ), "b"((uint32_t)path), "c"((uint32_t)buf), "d"(max)
        : "memory");
    return ret;
}

int32_t sys_ramfs_write(const char* path, const char* buf, uint32_t size) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_RAMFS_WRITE), "b"((uint32_t)path), "c"((uint32_t)buf), "d"(size)
        : "memory");
    return ret;
}

int32_t sys_ramfs_create(const char* path, uint32_t type) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_RAMFS_CREATE), "b"((uint32_t)path), "c"(type)
        : "memory");
    return ret;
}

int32_t sys_ramfs_delete(const char* path) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_RAMFS_DELETE), "b"((uint32_t)path)
        : "memory");
    return ret;
}

int32_t sys_procfs_read(const char* path, char* buf, uint32_t max) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_PROCFS_READ), "b"((uint32_t)path), "c"((uint32_t)buf), "d"(max)
        : "memory");
    return ret;
}

int32_t sys_fat16_read(const char* path, uint8_t* buf, uint32_t max) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_FAT16_READ), "b"((uint32_t)path), "c"((uint32_t)buf), "d"(max)
        : "memory");
    return ret;
}

int32_t sys_fat16_write(const char* path, const uint8_t* buf, uint32_t size) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_FAT16_WRITE), "b"((uint32_t)path), "c"((uint32_t)buf), "d"(size)
        : "memory");
    return ret;
}

int32_t sys_fat16_mounted(void) {
    int32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_FAT16_MOUNTED));
    return ret;
}

int32_t sys_ntfs_read(const char* path, uint8_t* buf, uint32_t max) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_NTFS_READ), "b"((uint32_t)path), "c"((uint32_t)buf), "d"(max)
        : "memory");
    return ret;
}

int32_t sys_ntfs_write(const char* path, const uint8_t* buf, uint32_t size) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_NTFS_WRITE), "b"((uint32_t)path), "c"((uint32_t)buf), "d"(size)
        : "memory");
    return ret;
}

int32_t sys_ntfs_mounted(void) {
    int32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_NTFS_MOUNTED));
    return ret;
}

int32_t sys_ata_read(uint32_t lba, uint8_t* buf, uint32_t count) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_ATA_READ), "b"(lba), "c"((uint32_t)buf), "d"(count)
        : "memory");
    return ret;
}

int32_t sys_ata_write(uint32_t lba, const uint8_t* buf, uint32_t count) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_ATA_WRITE), "b"(lba), "c"((uint32_t)buf), "d"(count)
        : "memory");
    return ret;
}

int32_t sys_ata_info(uint32_t drive) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_ATA_INFO), "b"(drive));
    return ret;
}

int32_t sys_net_status(void) {
    int32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_NET_STATUS));
    return ret;
}

int32_t sys_net_poll(void) {
    int32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_NET_POLL));
    return ret;
}

void sys_debug_log(const char* msg) {
    __asm__ volatile ("int $0x80"
        : : "a"(SYS_DEBUG_LOG), "b"((uint32_t)msg) : "memory");
}

uint32_t sys_gui_win_open(const char* title) {
    uint32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_GUI_WIN_OPEN), "b"((uint32_t)title)
        : "memory");
    return ret;
}

void sys_gui_present(void) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_GUI_PRESENT));
}

void sys_gui_win_close(void) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_GUI_WIN_CLOSE));
}

uint32_t sys_gui_get_ticks(void) {
    uint32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_GUI_GET_TICKS));
    return ret;
}


void sys_sleep(uint32_t ms) {
    __asm__ volatile ("int $0x80" 
        : 
        : "a"(SYS_SLEEP), "b"(ms) 
        : "memory");
}

 uint32_t sys_get_ticks(void) {
    uint32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(7)  /* SYS_GET_TICKS = 7 */
    );
    return ret;
}

/* --- Inside src/userlib.c --- */

/**
 * Syscall wrapper for VirGL command submission
 * EAX: Syscall Number
 * EBX: Pointer to command buffer
 * ECX: Size of buffer
 */
int32_t sys_virgl_submit(void* cmd_buf, uint32_t size) {
    int32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_VIRGL_SUBMIT), "b"((uint32_t)cmd_buf), "c"(size)
        : "memory"
    );
    return ret;
}

/* --- GPU 3D high-level syscall wrappers --- */

int32_t sys_gpu3d_init(uint32_t width, uint32_t height) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_GPU3D_INIT), "b"(width), "c"(height)
        : "memory");
    return ret;
}

int32_t sys_gpu3d_clear(uint32_t flags, uint32_t color_argb) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_GPU3D_CLEAR), "b"(flags), "c"(color_argb)
        : "memory");
    return ret;
}

int32_t sys_gpu3d_upload(const float* vertices, uint32_t num_floats) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_GPU3D_UPLOAD), "b"((uint32_t)vertices), "c"(num_floats)
        : "memory");
    return ret;
}

int32_t sys_gpu3d_set_mvp(const float* mvp16) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_GPU3D_MVP), "b"((uint32_t)mvp16)
        : "memory");
    return ret;
}

int32_t sys_gpu3d_draw(uint32_t prim_mode, uint32_t start, uint32_t count) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_GPU3D_DRAW), "b"(prim_mode), "c"(start), "d"(count)
        : "memory");
    return ret;
}

int32_t sys_gpu3d_present(void) {
    int32_t ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_GPU3D_PRESENT)
        : "memory");
    return ret;
}

/**
 * Simple utoa (Unsigned Integer to ASCII) implementation
 */
char* utoa(uint32_t value, char* str, int base) {
    char *rc;
    char *ptr;
    char *low;
    // Check for supported base
    if (base < 2 || base > 36) {
        *str = '\0';
        return str;
    }
    rc = ptr = str;
    // Set pointer to last digit
    do {
        *ptr++ = "0123456789abcdefghijklmnopqrstuvwxyz"[value % base];
        value /= base;
    } while (value);
    // Terminate string
    *ptr-- = '\0';
    // Invert the box
    low = rc;
    while (low < ptr) {
        char tmp = *low;
        *low++ = *ptr;
        *ptr-- = tmp;
    }
    return rc;
}