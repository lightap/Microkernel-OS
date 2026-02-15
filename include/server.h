#ifndef SERVER_H
#define SERVER_H

#include "types.h"
#include "ipc.h"

/*
 * Microkernel server framework.
 *
 * Each server runs as a ring 3 task with its own address space.
 * Servers communicate with each other and with user processes
 * through synchronous IPC messages.
 *
 * Server lifecycle:
 *   1. Register service name (e.g., "console", "vfs", "ata")
 *   2. Optionally register for IRQ notifications
 *   3. Enter receive loop: receive message, process, reply
 *
 * Client pattern:
 *   1. Look up service PID: pid = sys_lookup_service("vfs")
 *   2. Send request: sys_sendrec(pid, &msg)
 *   3. Result is in msg when sendrec returns
 */

/* Well-known service names */
#define SVC_CONSOLE     "console"
#define SVC_VFS         "vfs"
#define SVC_DISK        "ata"
#define SVC_NET         "net"

/* Server entry points — launched as ring 3 tasks by the kernel */
void console_server_main(void);
void vfs_server_main(void);
void disk_server_main(void);
void net_server_main(void);

/* Server PIDs — set during boot, used by clients for IPC */
extern uint32_t console_server_pid;
extern uint32_t vfs_server_pid;
extern uint32_t disk_server_pid;
extern uint32_t net_server_pid;

/* Wait for all core servers to be registered */
void servers_wait_ready(void);

/* Launch all servers — called from kernel_main */
void servers_launch(void);

/* Launch only servers not already loaded from ELF modules */
void servers_launch_missing(void);

#endif
