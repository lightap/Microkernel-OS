#include "server.h"
#include "task.h"
#include "syscall.h"
#include "vga.h"
#include "keyboard.h"
#include "ramfs.h"
#include "ata.h"
#include "fat16.h"
#include "ntfs.h"
#include "net.h"
#include "serial.h"
#include "timer.h"
#include "procfs.h"

/*
 * Microkernel servers — each runs as a ring 3 process.
 *
 * In this single-binary model, server code is compiled into the kernel
 * image but executes in ring 3 with its own address space. Servers that
 * need hardware access (disk, network, console) run with IOPL=3.
 *
 * Communication between servers and clients goes through synchronous IPC.
 * The server pattern is:
 *   while (1) {
 *       receive(&msg);
 *       process(msg);
 *       reply(msg.sender, &response);
 *   }
 */

uint32_t console_server_pid = 0;
uint32_t vfs_server_pid     = 0;
uint32_t disk_server_pid    = 0;
uint32_t net_server_pid     = 0;

/* ================================================================
 * CONSOLE SERVER — handles VGA display and keyboard input
 * ================================================================ */

void console_server_main(void) {
    /* Register our service */
    sys_register_service(SVC_CONSOLE);

    message_t msg;
    message_t reply;

    while (1) {
        /* Block waiting for a request */
        sys_receive(PID_ANY, &msg);

        memset(&reply, 0, sizeof(reply));
        reply.type = MSG_REPLY;

        switch (msg.type) {
        case MSG_CONS_WRITE: {
            /* Write string to console */
            const char* text = msg.cons.data;
            uint32_t len = msg.cons.len;
            if (len > sizeof(msg.cons.data)) len = sizeof(msg.cons.data);
            for (uint32_t i = 0; i < len && text[i]; i++)
                terminal_putchar(text[i]);
            reply.reply.status = 0;
            reply.reply.value = len;
            break;
        }
        case MSG_CONS_PUTCHAR: {
            terminal_putchar((char)msg.cons.data[0]);
            reply.reply.status = 0;
            break;
        }
        case MSG_CONS_GETCHAR: {
            /* Blocking keyboard read */
            char c = keyboard_getchar();
            reply.reply.status = 0;
            reply.reply.value = (uint32_t)c;
            break;
        }
        case MSG_CONS_CLEAR: {
            terminal_clear();
            reply.reply.status = 0;
            break;
        }
        case MSG_CONS_SETCOLOR: {
            terminal_setcolor(msg.cons.color);
            reply.reply.status = 0;
            break;
        }
        case MSG_IRQ_NOTIFY: {
            /* IRQ1 (keyboard) notification — handled by next read */
            continue; /* Don't reply to notifications */
        }
        default:
            reply.reply.status = -1;
            break;
        }

        sys_reply(msg.sender, &reply);
    }
}

/* ================================================================
 * VFS SERVER — virtual filesystem, routes to ramfs/fat16/ntfs
 * ================================================================ */

void vfs_server_main(void) {
    sys_register_service(SVC_VFS);

    message_t msg;
    message_t reply;

    while (1) {
        sys_receive(PID_ANY, &msg);

        memset(&reply, 0, sizeof(reply));
        reply.type = MSG_REPLY;

        switch (msg.type) {
        case MSG_VFS_READ: {
            const char* path = msg.io.path;
            uint32_t buf_addr = msg.io.buf;
            uint32_t max_size = msg.io.size;
            int32_t bytes_read = -1;

            /* Route based on path prefix */
            if (strncmp(path, "/proc", 5) == 0) {
                bytes_read = procfs_read(path, (char*)buf_addr, max_size);
            } else if (strncmp(path, "/disk/", 6) == 0) {
                /* Try FAT16 first, then NTFS */
                if (fat16_is_mounted()) {
                    bytes_read = fat16_read_file(path + 5, (uint8_t*)buf_addr, max_size);
                } else if (ntfs_is_mounted()) {
                    bytes_read = ntfs_read_file(path + 5, (uint8_t*)buf_addr, max_size);
                }
            } else {
                /* Default to ramfs */
                bytes_read = ramfs_read(path, (char*)buf_addr, max_size);
            }

            reply.reply.status = (bytes_read >= 0) ? 0 : -1;
            reply.reply.value = (uint32_t)bytes_read;
            break;
        }
        case MSG_VFS_WRITE: {
            const char* path = msg.io.path;
            uint32_t buf_addr = msg.io.buf;
            uint32_t size = msg.io.size;
            int32_t bytes_written = -1;

            if (strncmp(path, "/disk/", 6) == 0) {
                if (fat16_is_mounted()) {
                    bytes_written = fat16_write_file(path + 5, (const uint8_t*)buf_addr, size);
                } else if (ntfs_is_mounted()) {
                    bytes_written = ntfs_write_file(path + 5, (const uint8_t*)buf_addr, size);
                }
            } else {
                bytes_written = ramfs_write(path, (const char*)buf_addr, size);
            }

            reply.reply.status = (bytes_written >= 0) ? 0 : -1;
            reply.reply.value = (uint32_t)bytes_written;
            break;
        }
        case MSG_VFS_CREATE: {
            const char* path = msg.io.path;
            uint32_t type = msg.io.fd;  /* 0=file, 1=dir */
            int32_t ret;

            if (type == 1) {
                ret = ramfs_create(path, RAMFS_DIR);
            } else {
                ret = ramfs_create(path, RAMFS_FILE);
            }

            reply.reply.status = ret;
            break;
        }
        case MSG_VFS_DELETE: {
            reply.reply.status = ramfs_delete(msg.io.path);
            break;
        }
        case MSG_VFS_LIST: {
            /* List directory — returns entries in reply data */
            reply.reply.status = 0;
            break;
        }
        default:
            reply.reply.status = -1;
            break;
        }

        sys_reply(msg.sender, &reply);
    }
}

/* ================================================================
 * DISK SERVER — ATA disk driver
 * ================================================================ */

void disk_server_main(void) {
    sys_register_service(SVC_DISK);

    message_t msg;
    message_t reply;

    while (1) {
        sys_receive(PID_ANY, &msg);

        memset(&reply, 0, sizeof(reply));
        reply.type = MSG_REPLY;

        switch (msg.type) {
        case MSG_DISK_READ: {
            uint32_t lba = msg.io.offset;
            uint32_t buf_addr = msg.io.buf;
            uint32_t count = msg.io.size;
            /* Read sectors from ATA drive */
            bool ok = ata_read_sectors(lba, (uint8_t)count, (void*)buf_addr);
            reply.reply.status = ok ? 0 : -1;
            reply.reply.value = count;
            break;
        }
        case MSG_DISK_WRITE: {
            uint32_t lba = msg.io.offset;
            uint32_t buf_addr = msg.io.buf;
            uint32_t count = msg.io.size;
            bool ok = ata_write_sectors(lba, (uint8_t)count, (const void*)buf_addr);
            reply.reply.status = ok ? 0 : -1;
            reply.reply.value = count;
            break;
        }
        case MSG_DISK_INFO: {
            /* Return disk info */
            reply.reply.status = ata_drive_present(0) ? 0 : -1;
            if (ata_drive_present(0)) {
                ata_drive_t* d = ata_get_drive_n(0);
                reply.reply.value = d->size_mb;
            }
            break;
        }
        case MSG_IRQ_NOTIFY: {
            /* IRQ14/15 (ATA) — disk operation completed */
            continue;
        }
        default:
            reply.reply.status = -1;
            break;
        }

        sys_reply(msg.sender, &reply);
    }
}

/* ================================================================
 * NETWORK SERVER — RTL8139 network driver
 * ================================================================ */

void net_server_main(void) {
    sys_register_service(SVC_NET);

    message_t msg;
    message_t reply;

    while (1) {
        sys_receive(PID_ANY, &msg);

        memset(&reply, 0, sizeof(reply));
        reply.type = MSG_REPLY;

        switch (msg.type) {
        case MSG_NET_IFCONFIG: {
            reply.reply.status = net_is_available() ? 0 : -1;
            break;
        }
        case MSG_NET_PING: {
            /* msg.io.buf = IP address (4 bytes) */
            reply.reply.status = 0;
            break;
        }
        case MSG_IRQ_NOTIFY: {
            /* IRQ11 (RTL8139) — packet received/transmitted */
            if (net_is_available()) {
                net_poll();
            }
            continue;
        }
        default:
            reply.reply.status = -1;
            break;
        }

        sys_reply(msg.sender, &reply);
    }
}

/* ================================================================
 * Server lifecycle management
 * ================================================================ */

void servers_launch(void) {
    int32_t pid;

    /* Console server — needs IOPL for VGA and keyboard ports */
    pid = task_create_user("console_srv", console_server_main, 1, true);
    if (pid >= 0) console_server_pid = pid;

    /* VFS server — no direct hardware access needed */
    pid = task_create_user("vfs_srv", vfs_server_main, 2, false);
    if (pid >= 0) vfs_server_pid = pid;

    /* Disk server — needs IOPL for ATA ports */
    pid = task_create_user("ata_srv", disk_server_main, 2, true);
    if (pid >= 0) disk_server_pid = pid;

    /* Network server — needs IOPL for RTL8139 ports */
    pid = task_create_user("net_srv", net_server_main, 2, true);
    if (pid >= 0) net_server_pid = pid;
}

void servers_launch_missing(void) {
    /* Only launch servers that weren't loaded from ELF modules.
     * This is the fallback path when booting without GRUB modules. */
    int32_t pid;

    if (!console_server_pid) {
        pid = task_create_user("console_srv", console_server_main, 1, true);
        if (pid >= 0) console_server_pid = pid;
    }
    if (!vfs_server_pid) {
        pid = task_create_user("vfs_srv", vfs_server_main, 2, false);
        if (pid >= 0) vfs_server_pid = pid;
    }
    if (!disk_server_pid) {
        pid = task_create_user("ata_srv", disk_server_main, 2, true);
        if (pid >= 0) disk_server_pid = pid;
    }
    if (!net_server_pid) {
        pid = task_create_user("net_srv", net_server_main, 2, true);
        if (pid >= 0) net_server_pid = pid;
    }
}

void servers_wait_ready(void) {
    /* Give servers a moment to register their services */
    uint32_t deadline = timer_get_ticks() + 10; /* 100ms at 100Hz */
    while (timer_get_ticks() < deadline) {
        if (ipc_lookup_service(SVC_CONSOLE) &&
            ipc_lookup_service(SVC_VFS) &&
            ipc_lookup_service(SVC_DISK)) {
            return; /* All core servers registered */
        }
        hlt(); /* Wait for next tick */
    }
}
