#ifndef IPC_H
#define IPC_H

#include "types.h"

/*
 * Synchronous IPC — the microkernel communication backbone.
 *
 * All inter-process communication in the microkernel goes through
 * blocking send/receive. This is the Minix/L4 model:
 *   - send() blocks until the receiver calls receive()
 *   - receive() blocks until a sender calls send()
 *   - sendrec() sends then blocks for a reply (most common pattern)
 *   - reply() responds to a received message (non-blocking)
 *   - notify() delivers a lightweight async notification (for IRQs)
 */

/* Message types — servers register what they handle */
#define MSG_TYPE_NONE       0

/* Console server messages */
#define MSG_CONS_WRITE      10
#define MSG_CONS_READ       11
#define MSG_CONS_PUTCHAR    12
#define MSG_CONS_CLEAR      13
#define MSG_CONS_SETCOLOR   14
#define MSG_CONS_GETCHAR    15

/* VFS server messages */
#define MSG_VFS_READ        20
#define MSG_VFS_WRITE       21
#define MSG_VFS_CREATE      22
#define MSG_VFS_DELETE      23
#define MSG_VFS_LIST        24
#define MSG_VFS_STAT        25
#define MSG_VFS_MKDIR       26

/* Disk server messages */
#define MSG_DISK_READ       30
#define MSG_DISK_WRITE      31
#define MSG_DISK_INFO       32

/* Network server messages */
#define MSG_NET_SEND        40
#define MSG_NET_RECV        41
#define MSG_NET_IFCONFIG    42
#define MSG_NET_PING        43
#define MSG_NET_DNS         44

/* System messages */
#define MSG_IRQ_NOTIFY      100
#define MSG_REPLY           101
#define MSG_REGISTER        102
#define MSG_PING            103

/* Special PIDs */
#define PID_ANY         0xFFFFFFFF  /* Receive from any sender */

/* IPC message — 64 bytes, fits in a cache line */
typedef struct message {
    uint32_t sender;        /* Filled by kernel on delivery */
    uint32_t type;          /* Message type (MSG_*) */
    union {
        uint8_t raw[56];    /* Raw data */
        struct {            /* I/O request */
            uint32_t fd;
            uint32_t offset;
            uint32_t size;
            uint32_t buf;   /* User-space buffer address */
            char     path[40];
        } io;
        struct {            /* IRQ notification */
            uint32_t irq;
            uint32_t ticks;
        } interrupt;
        struct {            /* Reply */
            int32_t  status;
            uint32_t value;
            uint32_t size;
            uint32_t buf;
            uint8_t  data[40];
        } reply;
        struct {            /* Service registration */
            char     name[52];
            uint32_t pid;
        } reg;
        struct {            /* Console I/O */
            uint32_t len;
            uint8_t  color;
            char     data[47];
        } cons;
    };
} message_t;

/* Blocking reasons for tasks */
#define BLOCKED_NONE     0
#define BLOCKED_SEND     1   /* Waiting for receiver to accept */
#define BLOCKED_RECEIVE  2   /* Waiting for any sender */
#define BLOCKED_SENDREC  3   /* Sent message, waiting for reply */

/* Kernel-side IPC functions (called from syscall handler) */
void    ipc_init(void);
int32_t ipc_send(uint32_t dest_pid, message_t* msg);
int32_t ipc_receive(uint32_t from_pid, message_t* msg);
int32_t ipc_sendrec(uint32_t dest_pid, message_t* msg);
int32_t ipc_reply(uint32_t dest_pid, message_t* msg);
int32_t ipc_notify(uint32_t dest_pid, message_t* msg);

/* Service registry — maps names to PIDs */
#define MAX_SERVICES 16
#define SERVICE_NAME_LEN 32

int32_t  ipc_register_service(const char* name, uint32_t pid);
uint32_t ipc_lookup_service(const char* name);
void     ipc_service_list(void);

/* Stats */
uint32_t ipc_message_count(void);
void     ipc_status(void);

/* Legacy compatibility — port-based API (deprecated, wraps new IPC) */
int32_t ipc_create_port(const char* name, uint32_t owner);
int32_t ipc_find_port(const char* name);
uint32_t ipc_port_count(void);

#endif
