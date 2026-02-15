#include "ipc.h"
#include "task.h"
#include "vga.h"
#include "serial.h"
#include "heap.h"

/*
 * Synchronous IPC implementation.
 *
 * The core microkernel primitive. All communication between servers
 * and between user processes and servers goes through this.
 *
 * send(dest, msg): blocks until dest calls receive()
 * receive(from, msg): blocks until someone sends to us
 * sendrec(dest, msg): send then wait for reply
 * reply(dest, msg): non-blocking reply to a sender
 * notify(dest, msg): non-blocking notification (for IRQs)
 */

/* Service registry */
typedef struct {
    bool     active;
    char     name[SERVICE_NAME_LEN];
    uint32_t pid;
} service_entry_t;

static service_entry_t services[MAX_SERVICES];
static uint32_t total_messages = 0;

/* Pending notification bitmap per task */
static uint32_t pending_notify[MAX_TASKS];
static message_t notify_msg[MAX_TASKS];

void ipc_init(void) {
    memset(services, 0, sizeof(services));
    memset(pending_notify, 0, sizeof(pending_notify));
    total_messages = 0;
}

/* Copy a message between two buffers */
static void copy_message(const message_t* src, message_t* dst) {
    memcpy(dst, src, sizeof(message_t));
}

/* Find a task that is blocked trying to send to 'receiver_pid' */
static task_t* find_blocked_sender(uint32_t receiver_pid, uint32_t from_pid) {
    task_t* all = task_get_all();
    for (int i = 0; i < MAX_TASKS; i++) {
        if (!all[i].active) continue;
        if (all[i].state != TASK_BLOCKED) continue;
        if (all[i].blocked_on != BLOCKED_SEND && all[i].blocked_on != BLOCKED_SENDREC) continue;
        if (all[i].send_to != receiver_pid) continue;
        if (from_pid != PID_ANY && all[i].id != from_pid) continue;
        return &all[i];
    }
    return NULL;
}

int32_t ipc_send(uint32_t dest_pid, message_t* msg) {
    task_t* current = task_get_current();
    if (!current || !msg) return -1;

    task_t* dest = task_get_by_pid(dest_pid);
    if (!dest || !dest->active) return -2; /* No such process */

    msg->sender = current->id;
    total_messages++;

    /* Is the destination already waiting to receive from us (or from ANY)? */
    if (dest->state == TASK_BLOCKED && dest->blocked_on == BLOCKED_RECEIVE) {
        uint32_t from = dest->receive_from;
        if (from == PID_ANY || from == current->id) {
            /* Deliver immediately */
            copy_message(msg, dest->msg_buf);
            dest->msg_buf->sender = current->id;
            dest->state = TASK_READY;
            dest->blocked_on = BLOCKED_NONE;
            return 0;
        }
    }

    /* Destination not ready — block the sender */
    current->state = TASK_BLOCKED;
    current->blocked_on = BLOCKED_SEND;
    current->send_to = dest_pid;
    current->msg_buf = msg;
    task_yield();

    /* When we resume, the message was delivered */
    return 0;
}

int32_t ipc_receive(uint32_t from_pid, message_t* msg) {
    task_t* current = task_get_current();
    if (!current || !msg) return -1;

    /* Check for pending notification first */
    if (pending_notify[current->id % MAX_TASKS]) {
        copy_message(&notify_msg[current->id % MAX_TASKS], msg);
        pending_notify[current->id % MAX_TASKS] = 0;
        return 0;
    }

    /* Check if someone is already blocked trying to send to us */
    task_t* sender = find_blocked_sender(current->id, from_pid);
    if (sender) {
        copy_message(sender->msg_buf, msg);
        msg->sender = sender->id;

        if (sender->blocked_on == BLOCKED_SENDREC) {
            /* Sender is doing sendrec — keep it blocked, waiting for our reply */
            sender->blocked_on = BLOCKED_SENDREC;
            sender->state = TASK_BLOCKED;
        } else {
            /* Plain send — unblock the sender */
            sender->state = TASK_READY;
            sender->blocked_on = BLOCKED_NONE;
        }
        return 0;
    }

    /* No pending message — block and wait */
    current->state = TASK_BLOCKED;
    current->blocked_on = BLOCKED_RECEIVE;
    current->receive_from = from_pid;
    current->msg_buf = msg;
    task_yield();

    /* When we resume, the message was delivered to our msg_buf */
    return 0;
}

int32_t ipc_sendrec(uint32_t dest_pid, message_t* msg) {
    task_t* current = task_get_current();
    if (!current || !msg) return -1;

    task_t* dest = task_get_by_pid(dest_pid);
    if (!dest || !dest->active) return -2;

    msg->sender = current->id;
    total_messages++;

    /* Is the destination waiting to receive? */
    if (dest->state == TASK_BLOCKED && dest->blocked_on == BLOCKED_RECEIVE) {
        uint32_t from = dest->receive_from;
        if (from == PID_ANY || from == current->id) {
            /* Deliver the send part immediately */
            copy_message(msg, dest->msg_buf);
            dest->msg_buf->sender = current->id;
            dest->state = TASK_READY;
            dest->blocked_on = BLOCKED_NONE;

            /* Now block ourselves waiting for the reply */
            current->state = TASK_BLOCKED;
            current->blocked_on = BLOCKED_SENDREC;
            current->send_to = dest_pid;
            current->msg_buf = msg;  /* Reply will be written here */
            task_yield();
            return 0;
        }
    }

    /* Destination not ready — block as SENDREC */
    current->state = TASK_BLOCKED;
    current->blocked_on = BLOCKED_SENDREC;
    current->send_to = dest_pid;
    current->msg_buf = msg;
    task_yield();
    return 0;
}

int32_t ipc_reply(uint32_t dest_pid, message_t* msg) {
    task_t* current = task_get_current();
    if (!current || !msg) return -1;

    task_t* dest = task_get_by_pid(dest_pid);
    if (!dest || !dest->active) return -2;

    /* The destination must be blocked in SENDREC waiting for our reply */
    if (dest->state == TASK_BLOCKED && dest->blocked_on == BLOCKED_SENDREC) {
        msg->sender = current->id;
        copy_message(msg, dest->msg_buf);
        dest->state = TASK_READY;
        dest->blocked_on = BLOCKED_NONE;
        total_messages++;
        return 0;
    }

    /* Destination not waiting for reply — error */
    return -3;
}

int32_t ipc_notify(uint32_t dest_pid, message_t* msg) {
    task_t* dest = task_get_by_pid(dest_pid);
    if (!dest || !dest->active) return -2;

    msg->sender = 0; /* Kernel/IRQ notification */
    total_messages++;

    /* If destination is blocked in receive, deliver immediately */
    if (dest->state == TASK_BLOCKED && dest->blocked_on == BLOCKED_RECEIVE) {
        copy_message(msg, dest->msg_buf);
        dest->state = TASK_READY;
        dest->blocked_on = BLOCKED_NONE;
        return 0;
    }

    /* Otherwise store as pending notification */
    uint32_t idx = dest_pid % MAX_TASKS;
    copy_message(msg, &notify_msg[idx]);
    pending_notify[idx] = 1;
    return 0;
}

/* --- Service registry --- */

int32_t ipc_register_service(const char* name, uint32_t pid) {
    /* Check for duplicates */
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (services[i].active && strcmp(services[i].name, name) == 0) {
            services[i].pid = pid;
            return i;
        }
    }
    /* Find free slot */
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (!services[i].active) {
            services[i].active = true;
            strncpy(services[i].name, name, SERVICE_NAME_LEN - 1);
            services[i].name[SERVICE_NAME_LEN - 1] = '\0';
            services[i].pid = pid;
            return i;
        }
    }
    return -1;
}

uint32_t ipc_lookup_service(const char* name) {
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (services[i].active && strcmp(services[i].name, name) == 0)
            return services[i].pid;
    }
    return 0;
}

void ipc_service_list(void) {
    kprintf("  SERVICE            PID\n");
    kprintf("  -------            ---\n");
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (services[i].active) {
            kprintf("  %-20s %u\n", services[i].name, services[i].pid);
        }
    }
}

/* --- Stats --- */

uint32_t ipc_message_count(void) { return total_messages; }

void ipc_status(void) {
    kprintf("  IPC Statistics:\n");
    kprintf("    Total messages: %u\n", total_messages);
    kprintf("\n");
    ipc_service_list();
}

uint32_t ipc_port_count(void) {
    uint32_t c = 0;
    for (int i = 0; i < MAX_SERVICES; i++)
        if (services[i].active) c++;
    return c;
}

/* Legacy compatibility */
int32_t ipc_create_port(const char* name, uint32_t owner) {
    return ipc_register_service(name, owner);
}

int32_t ipc_find_port(const char* name) {
    uint32_t pid = ipc_lookup_service(name);
    return pid ? (int32_t)pid : -1;
}
