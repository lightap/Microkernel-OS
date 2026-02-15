#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"

/*
 * Syscall numbers for the microkernel.
 *
 * The minimal microkernel syscall interface:
 *   - IPC (send, receive, sendrec, reply, notify)
 *   - Task management (exit, getpid, sleep)
 *   - Memory (malloc, free - temporary, will be replaced by VMM grants)
 *   - I/O privileges (grant_io, register_irq)
 *   - Service registry (register_service, lookup_service)
 *   - Legacy I/O (write, read - for direct console during boot)
 */

/* Legacy / direct I/O (used during early boot before servers are up) */
#define SYS_WRITE           1
#define SYS_READ            2
#define SYS_OPEN            3
#define SYS_CLOSE           4
#define SYS_GETPID          5
#define SYS_EXIT            6
#define SYS_SLEEP           7
#define SYS_TIME            8
#define SYS_MALLOC          9
#define SYS_FREE            10
#define SYS_EXEC            11
#define SYS_GET_TIME        12
#define SYS_GET_TICKS       13   /* NEW: Get timer ticks */
/* IPC syscalls — the core microkernel interface */
#define SYS_SEND            20  /* Send message, block until received */
#define SYS_RECEIVE         21  /* Block until message arrives */
#define SYS_SENDREC         22  /* Send then wait for reply */
#define SYS_REPLY           23  /* Reply to a received message */
#define SYS_NOTIFY          24  /* Non-blocking notification */

/* Microkernel service syscalls */
#define SYS_REGISTER_SVC    30  /* Register a named service */
#define SYS_LOOKUP_SVC      31  /* Look up a service PID by name */
#define SYS_GRANT_IO        32  /* Grant I/O port access to a task */
#define SYS_REGISTER_IRQ    33  /* Register to receive IRQ notifications */
#define SYS_CREATE_TASK     34  /* Create a new ring 3 task */

/* Server-support syscalls — allow isolated servers to access kernel services.
 * These bridge the gap until drivers are fully self-contained in userspace.
 * Servers call these via INT 0x80 like any other syscall. */
#define SYS_KBD_GETCHAR     40  /* Read a character from keyboard buffer */
#define SYS_RAMFS_READ      41  /* Read from ramfs file */
#define SYS_RAMFS_WRITE     42  /* Write to ramfs file */
#define SYS_RAMFS_CREATE    43  /* Create ramfs file or directory */
#define SYS_RAMFS_DELETE    44  /* Delete ramfs entry */
#define SYS_PROCFS_READ     45  /* Read from /proc */
#define SYS_FAT16_READ      46  /* Read from FAT16 file */
#define SYS_FAT16_WRITE     47  /* Write to FAT16 file */
#define SYS_FAT16_MOUNTED   48  /* Check if FAT16 is mounted */
#define SYS_NTFS_READ       49  /* Read from NTFS file */
#define SYS_NTFS_WRITE      50  /* Write to NTFS file */
#define SYS_NTFS_MOUNTED    51  /* Check if NTFS is mounted */
#define SYS_ATA_READ        52  /* Read ATA sectors */
#define SYS_ATA_WRITE       53  /* Write ATA sectors */
#define SYS_ATA_INFO        54  /* Get ATA drive info */
#define SYS_NET_STATUS      55  /* Get network status */
#define SYS_NET_POLL        56  /* Poll network device */
#define SYS_DEBUG_LOG       57  /* Write to serial debug log */

/* GUI window syscalls — allow ELF processes to open GUI windows */
/* GUI window syscalls — allow ELF processes to open GUI windows */
#define SYS_GUI_WIN_OPEN    60  /* Open a GUI window with shared framebuffer */
#define SYS_GUI_PRESENT     61  /* Signal framebuffer is ready, yield */
#define SYS_GUI_WIN_CLOSE   62  /* Close the GUI window */
#define SYS_GUI_GET_TICKS   63  /* Get timer ticks (for animation) */

/* Virgl/virtio-gpu 3D command submission */
#define SYS_VIRGL_SUBMIT    70

/* GPU 3D high-level API — wraps kernel virgl for user-space programs */
#define SYS_GPU3D_INIT      71  /* Init virgl + framebuffer + pipeline (ebx=w, ecx=h) */
#define SYS_GPU3D_CLEAR     72  /* Clear buffers (ebx=flags, ecx=color_packed_ARGB) */
#define SYS_GPU3D_UPLOAD    73  /* Upload vertices (ebx=ptr to floats, ecx=num_floats) */
#define SYS_GPU3D_MVP       74  /* Set MVP matrix (ebx=ptr to 16 floats) */
#define SYS_GPU3D_DRAW      75  /* Draw (ebx=prim_mode, ecx=start, edx=count) */
#define SYS_GPU3D_PRESENT   76  /* Present framebuffer to display */
void syscall_init(void);

/* --- User-space IPC wrappers (used by servers and user processes) --- */
int32_t sys_send(uint32_t dest, void* msg);
int32_t sys_receive(uint32_t from, void* msg);
int32_t sys_sendrec(uint32_t dest, void* msg);
int32_t sys_reply(uint32_t dest, void* msg);
int32_t sys_notify(uint32_t dest, void* msg);

/* Legacy wrappers */
int32_t sys_write(const char* buf, uint32_t len);
int32_t sys_read(char* buf, uint32_t len);
int32_t sys_getpid(void);
void    sys_exit(int code);
void    sys_sleep(uint32_t ms);

/* Service wrappers */
int32_t sys_register_service(const char* name);
uint32_t sys_lookup_service(const char* name);


/* Add this function declaration: */
uint32_t sys_get_time(void);

/* If you want millisecond precision, you could also add: */
uint32_t sys_get_time_ms(void);  /* Returns milliseconds since boot */

#endif
