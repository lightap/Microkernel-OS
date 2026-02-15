#ifndef TASK_H
#define TASK_H

#include "types.h"
#include "idt.h"

#define MAX_TASKS         32
#define TASK_STACK_SIZE   8192
#define KERNEL_STACK_SIZE 4096     /* Per-task kernel stack for ring 3 tasks */
#define TASK_NAME_LEN     32
#define DEFAULT_QUANTUM   10      /* Timer ticks per time slice (100ms at 100Hz) */

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_BLOCKED,
    TASK_TERMINATED
} task_state_t;

typedef struct {
    uint32_t eax, ebx, ecx, edx;
    uint32_t esi, edi, ebp, esp;
    uint32_t eip, eflags;
} task_regs_t;

/* Include IPC header for message_t */
#include "ipc.h"

typedef struct {
    uint32_t     id;
    char         name[TASK_NAME_LEN];
    task_state_t state;
    task_regs_t  regs;
    uint32_t     stack_base;
    uint32_t     stack_size;
    uint32_t     wake_tick;
    uint32_t     cpu_ticks;
    uint32_t     priority;
    bool         active;

    /* Preemptive scheduling */
    uint32_t     quantum;
    uint32_t     ticks_left;
    uint32_t     saved_esp;           /* Saved interrupt frame ESP */
    uint32_t     total_ticks;
    uint32_t     switches;

    /* --- Microkernel additions --- */

    /* Per-process address space */
    uint32_t*    page_directory;      /* Process page directory (NULL = kernel PD) */

    /* Ring 3 support */
    bool         is_user;             /* true = ring 3, false = ring 0 */
    uint32_t     kernel_stack_base;   /* Kernel stack allocation base */
    uint32_t     kernel_stack_top;    /* Top of kernel stack (TSS esp0) */

    /* IPC blocking state */
    uint32_t     blocked_on;          /* BLOCKED_NONE/SEND/RECEIVE/SENDREC */
    uint32_t     send_to;            /* PID we're trying to send to */
    uint32_t     receive_from;       /* PID we're waiting to receive from */
    message_t*  msg_buf;         /* Pointer to message buffer */

    /* Server capabilities */
    bool         io_privileged;       /* Has IOPL=3 for hardware access */
    uint32_t     owned_irqs;         /* Bitmask of IRQs this task handles */

    /* Kernel-side IPC message buffer.
     * With isolated address spaces, we can't write directly to a destination
     * task's user buffer (wrong page directory). So IPC copies go through
     * these kernel-resident buffers which are always accessible. */
    message_t    ipc_msg;
} task_t;

typedef void (*task_entry_t)(void);

/* Core scheduler */
void     task_init(void);
int32_t  task_create(const char* name, task_entry_t entry, uint32_t priority);
void     task_yield(void);
void     task_sleep(uint32_t ms);
void     task_exit(void);
void     task_kill(uint32_t id);
task_t*  task_get_current(void);
uint32_t task_count(void);
void     task_list(void);
void     task_schedule(void);

/* Preemptive scheduling */
void     task_timer_tick(registers_t* regs);
uint32_t task_preempt_check(registers_t* regs);
void     task_set_quantum(uint32_t ticks);
uint32_t task_get_quantum(void);
bool     task_is_preemptive(void);
void     task_set_preemptive(bool enabled);
void     task_lock_scheduler(void);
void     task_unlock_scheduler(void);

/* Microkernel task creation */
int32_t  task_create_user(const char* name, task_entry_t entry, uint32_t priority, bool io_privileged);

/* ELF-based task creation (isolated address space) */
int32_t  task_create_from_elf(const char* name, uint32_t entry_point,
                              uint32_t user_stack_top, uint32_t* page_dir,
                              uint32_t kstack_base, uint32_t kstack_size,
                              uint32_t priority, bool io_privileged);

/* Task lookup (for IPC) */
task_t*  task_get_by_pid(uint32_t pid);
task_t*  task_get_all(void);
uint32_t task_total_switches(void);

#endif
