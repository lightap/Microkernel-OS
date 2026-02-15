#include "syscall.h"
#include "idt.h"
#include "vga.h"
#include "keyboard.h"
#include "timer.h"
#include "task.h"
#include "heap.h"
#include "serial.h"
#include "ipc.h"
#include "gui.h"
#include "virgl.h"
#include "virgl_pipeline.h"

/*
 * Syscall handler — INT 0x80 entry point.
 *
 * For ring 3 tasks, the CPU automatically:
 *   1. Loads SS:ESP from TSS (switches to kernel stack)
 *   2. Pushes user SS, ESP, EFLAGS, CS, EIP
 *   3. Jumps here via the IDT
 *
 * Arguments are passed in registers (Linux convention):
 *   EAX = syscall number
 *   EBX = arg1
 *   ECX = arg2
 *   EDX = arg3
 *
 * Return value is placed in EAX.
 */

/* Common errno values used in syscalls */
#define ENOMEM   12   /* Out of memory */
#define EFAULT   14   /* Bad address / invalid pointer */
#define EINVAL   22   /* Invalid argument */

int copy_from_user(void* to, const void* from, uint32_t size);

int virgl_submit_command(const void* buf, uint32_t num_dwords);
/* IRQ ownership table — maps IRQ numbers to task PIDs */
static uint32_t irq_owners[16] = {0};

uint32_t irq_get_owner(uint32_t irq) {
    if (irq >= 16) return 0;
    return irq_owners[irq];
}

static void syscall_handler(registers_t* regs) {
    uint32_t num  = regs->eax;
    uint32_t arg1 = regs->ebx;
    uint32_t arg2 = regs->ecx;
    uint32_t arg3 = regs->edx;

    switch (num) {

    /* --- Legacy I/O (direct console, used during boot) --- */

    case SYS_WRITE: {
        const char* buf = (const char*)arg1;
        uint32_t len = arg2;
        for (uint32_t i = 0; i < len; i++)
            terminal_putchar(buf[i]);
        regs->eax = len;
        break;
    }
    case SYS_READ: {
        char* buf = (char*)arg1;
        uint32_t len = arg2;
        for (uint32_t i = 0; i < len; i++)
            buf[i] = keyboard_getchar();
        regs->eax = len;
        break;
    }
    case SYS_GETPID: {
        task_t* t = task_get_current();
        regs->eax = t ? t->id : 0;
        break;
    }
    case SYS_EXIT:
        task_exit();
        break;
    case SYS_SLEEP:
        task_sleep(arg1);
        regs->eax = 0;
        break;
    case SYS_TIME:
        regs->eax = timer_get_ticks();
        break;
    case SYS_GET_TICKS:
        regs->eax = timer_get_ticks();
        break;
    case SYS_MALLOC:
        regs->eax = (uint32_t)kmalloc(arg1);
        break;
    case SYS_FREE:
        kfree((void*)arg1);
        regs->eax = 0;
        break;

    /* --- IPC syscalls (the core microkernel interface) --- */

    case SYS_SEND: {
        /* arg1 = dest PID, arg2 = message_t* */
        regs->eax = (uint32_t)ipc_send(arg1, (message_t*)arg2);
        break;
    }
    case SYS_RECEIVE: {
        /* arg1 = from PID (or PID_ANY), arg2 = message_t* */
        regs->eax = (uint32_t)ipc_receive(arg1, (message_t*)arg2);
        break;
    }
    case SYS_SENDREC: {
        /* arg1 = dest PID, arg2 = message_t* (reply overwrites this) */
        regs->eax = (uint32_t)ipc_sendrec(arg1, (message_t*)arg2);
        break;
    }
    case SYS_REPLY: {
        /* arg1 = dest PID, arg2 = message_t* */
        regs->eax = (uint32_t)ipc_reply(arg1, (message_t*)arg2);
        break;
    }
    case SYS_NOTIFY: {
        /* arg1 = dest PID, arg2 = message_t* */
        regs->eax = (uint32_t)ipc_notify(arg1, (message_t*)arg2);
        break;
    }

    /* --- Microkernel service syscalls --- */

    case SYS_REGISTER_SVC: {
        /* arg1 = name string, arg2 = unused (uses caller's PID) */
        task_t* t = task_get_current();
        regs->eax = (uint32_t)ipc_register_service((const char*)arg1, t->id);
        break;
    }
    case SYS_LOOKUP_SVC: {
        /* arg1 = name string */
        regs->eax = ipc_lookup_service((const char*)arg1);
        break;
    }
    case SYS_GRANT_IO: {
        /* arg1 = target PID (0 = self) */
        /* For now, this is a privileged operation: only kernel (PID 0) can grant */
        task_t* caller = task_get_current();
        uint32_t target_pid = arg1 ? arg1 : caller->id;
        task_t* target = task_get_by_pid(target_pid);
        if (target) {
            target->io_privileged = true;
            regs->eax = 0;
        } else {
            regs->eax = (uint32_t)-1;
        }
        break;
    }
    case SYS_REGISTER_IRQ: {
        /* arg1 = IRQ number (0-15) */
        task_t* t = task_get_current();
        if (arg1 < 16) {
            irq_owners[arg1] = t->id;
            t->owned_irqs |= (1 << arg1);
            regs->eax = 0;
        } else {
            regs->eax = (uint32_t)-1;
        }
        break;
    }
    case SYS_CREATE_TASK: {
        /* arg1 = name, arg2 = entry point, arg3 = priority */
        /* Only privileged tasks can create new tasks */
        regs->eax = (uint32_t)task_create_user(
            (const char*)arg1, (task_entry_t)arg2, arg3, false);
        break;
    }
    case SYS_DEBUG_LOG: {
        /* arg1 = string pointer */
        const char* msg = (const char*)arg1;
        serial_printf("[PID %u] %s", task_get_current() ? task_get_current()->id : 0, msg);
        regs->eax = 0;
        break;
    }

    /* --- GUI window syscalls (for ELF processes) --- */

   case SYS_GUI_WIN_OPEN: {
        /* arg1 = title string pointer */
        task_t* t = task_get_current();
        if (t && t->page_directory) {
            regs->eax = gui_elf_win_open(t->id, t->page_directory, (const char*)arg1);
        } else {
            regs->eax = 0; /* Only ELF processes with own address space */
        }
        break;
    }

    

    case SYS_GUI_PRESENT: {
        task_t* t = task_get_current();
        if (t) {
            gui_elf_win_present(t->id);
        }
        regs->eax = 0; 
        break;
    }
    case SYS_GUI_WIN_CLOSE: {
        task_t* t = task_get_current();
        if (t) gui_elf_win_close(t->id);
        regs->eax = 0;
        break;
    }
    case SYS_GUI_GET_TICKS: {
        regs->eax = timer_get_ticks();
        break;
    }


    /* ──────────────────────── ADD RIGHT HERE ──────────────────────── */
case SYS_VIRGL_SUBMIT: {
        void* user_buf = (void*)arg1;
        uint32_t len   = arg2; // Length in bytes

        if (len == 0 || len > 128*1024 || !user_buf) {
            regs->eax = (uint32_t)-1;
            break;
        }

        uint32_t* kbuf = kmalloc(len);
        if (!kbuf) {
            regs->eax = (uint32_t)-ENOMEM;
            break;
        }

        if (copy_from_user(kbuf, user_buf, len) != 0) {
            kfree(kbuf);
            regs->eax = (uint32_t)-EFAULT;
            break;
        }

        /* 
         * Call the driver. 
         * Note: driver receives num_dwords, so divide bytes by 4.
         */
        int ret = virgl_submit_command(kbuf, len / 4);
        
        kfree(kbuf);
        regs->eax = (uint32_t)ret;
        break;
    }
    /* ──────────────────────────────────────────────────────────────── */

    /* ── GPU 3D High-Level API ── */

    case SYS_GPU3D_INIT: {
        /* arg1 = width, arg2 = height */
        uint32_t w = arg1 ? arg1 : 320;
        uint32_t h = arg2 ? arg2 : 200;
        serial_printf("GPU3D: init %ux%u\n", w, h);

        if (!virgl_init()) {
            serial_printf("GPU3D: virgl_init FAILED\n");
            regs->eax = (uint32_t)-1;
            break;
        }
        if (!virgl_setup_framebuffer((uint16_t)w, (uint16_t)h)) {
            serial_printf("GPU3D: framebuffer setup FAILED\n");
            regs->eax = (uint32_t)-1;
            break;
        }
       if (!virgl_setup_pipeline_state()) {
            serial_printf("GPU3D: pipeline setup FAILED\n");
            regs->eax = (uint32_t)-1;
            break;
        }

 // virgl_cmd_begin();
     virgl_cmd_set_viewport((uint16_t)w, (uint16_t)h);  // or w/h you stored
  // virgl_cmd_submit();
        serial_printf("GPU3D: init OK\n");
        regs->eax = 0;
        break;
    }

case SYS_GPU3D_CLEAR: {

      uint32_t w = arg1 ? arg1 : 320;
        uint32_t h = arg2 ? arg2 : 300;
    serial_printf("GPU3D_CLEAR: called, flags=%u color=%08x\n", arg1, arg2);
    uint32_t flags = arg1;
    
    uint32_t col = arg2;
    float r = (float)((col >> 16) & 0xFF) / 255.0f;
    float g = (float)((col >>  8) & 0xFF) / 255.0f;
    float b = (float)((col >>  0) & 0xFF) / 255.0f;
    float a = (float)((col >> 24) & 0xFF) / 255.0f;
    
    serial_printf("GPU3D_CLEAR: rgba=(%u,%u,%u,%u)/255\n",
                  (col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF, (col >> 24) & 0xFF);
 

 flags = arg1;

// If you have Z16 depth (no stencil), strip stencil bit
flags &= ~(PIPE_CLEAR_DEPTH | PIPE_CLEAR_STENCIL);


virgl_cmd_begin();
virgl_cmd_clear(flags, r, g, b, a, 1.0f, 0);
    serial_printf("GPU3D_CLEAR: submitting\n");
virgl_cmd_submit();



    serial_printf("GPU3D_CLEAR: done\n");
    regs->eax = 0;
    
    break;
}


case SYS_GPU3D_UPLOAD: {
    uint32_t num_floats = arg2;
    serial_printf("GPU3D_UPLOAD: called, num_floats=%u arg1=%p\n", num_floats, (void*)arg1);
    
    if (num_floats == 0 || num_floats > 32768 || !arg1) {
        serial_printf("GPU3D_UPLOAD: FAILED validation check\n");
        regs->eax = (uint32_t)-EINVAL;
        break;
    }
    
    uint32_t sz = num_floats * sizeof(float);
    float* kbuf = (float*)kmalloc(sz);
    if (!kbuf) { 
        serial_printf("GPU3D_UPLOAD: FAILED kmalloc\n");
        regs->eax = (uint32_t)-ENOMEM; 
        break; 
    }
    
    if (copy_from_user(kbuf, (void*)arg1, sz) != 0) {
        serial_printf("GPU3D_UPLOAD: FAILED copy_from_user\n");
        kfree(kbuf);
        regs->eax = (uint32_t)-EFAULT;
        break;
    }
    
    serial_printf("GPU3D_UPLOAD: uploading %u floats (%u bytes)\n", num_floats, sz);
    virgl_cmd_begin();
    virgl_upload_vertices(kbuf, num_floats);
    virgl_cmd_submit();
    kfree(kbuf);
    serial_printf("GPU3D_UPLOAD: SUCCESS\n");
    regs->eax = 0;
    break;
}
    case SYS_GPU3D_MVP: {
        /* arg1 = user ptr to 16 floats (4x4 matrix) */
        if (!arg1) { regs->eax = (uint32_t)-EINVAL; break; }
        float mvp[16];
        if (copy_from_user(mvp, (void*)arg1, 16 * sizeof(float)) != 0) {
            regs->eax = (uint32_t)-EFAULT;
            break;
        }
        virgl_cmd_begin();
        virgl_cmd_set_constant_buffer(PIPE_SHADER_VERTEX, mvp, 16);
        virgl_cmd_submit();
        regs->eax = 0;
        break;
    }
case SYS_GPU3D_DRAW: {
    /* arg1 = prim_mode (PIPE_PRIM_*), arg2 = start, arg3 = count */
    serial_printf("GPU3D_DRAW: mode=%u start=%u count=%u\n", arg1, arg2, arg3);
    virgl_cmd_begin();
    virgl_cmd_set_vertex_buffer(28, 0);  /* stride=28 (7 floats per vertex) */
    virgl_cmd_draw(arg1, arg2, arg3);
    virgl_cmd_submit();
    regs->eax = 0;
    break;
}

// src/syscall.c

    case SYS_GPU3D_PRESENT: {
        serial_printf("GPU3D_PRESENT: starting present\n");
        
        // 1. Perform the transfer from GPU to Guest RAM
        virgl_present();
        
        serial_printf("GPU3D_PRESENT: virgl_present done\n");
        
        // 2. Notify GUI that frame is ready
        task_t* t = task_get_current();
        gui_elf_win_present(t->id);
        
        /* 
         * 3. TARGET FIX FOR VISIBILITY:
         * Sleep for 1 tick (10ms). This limits the app to 100 FPS
         * and gives the GUI loop enough time to blit the frame
         * to the actual VGA screen.
         */
        t->state = TASK_SLEEPING;
        t->wake_tick = timer_get_ticks() + 1; 
        task_yield();
        
        regs->eax = 0;
        break;
    }

    default:
        serial_printf("Unknown syscall %d from PID %u\n",
                      num, task_get_current() ? task_get_current()->id : 0);
        regs->eax = (uint32_t)-1;
        break;
    }
}

void syscall_init(void) {
    register_interrupt_handler(0x80, syscall_handler);
}

/* ----------------------------------------------------------------
 * User-space IPC wrappers (INT 0x80)
 * ---------------------------------------------------------------- */

int32_t sys_send(uint32_t dest, void* msg) {
    int32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_SEND), "b"(dest), "c"((uint32_t)msg)
    );
    return ret;
}

int32_t sys_receive(uint32_t from, void* msg) {
    int32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_RECEIVE), "b"(from), "c"((uint32_t)msg)
    );
    return ret;
}

int32_t sys_sendrec(uint32_t dest, void* msg) {
    int32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_SENDREC), "b"(dest), "c"((uint32_t)msg)
    );
    return ret;
}

int32_t sys_reply(uint32_t dest, void* msg) {
    int32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_REPLY), "b"(dest), "c"((uint32_t)msg)
    );
    return ret;
}

int32_t sys_notify(uint32_t dest, void* msg) {
    int32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_NOTIFY), "b"(dest), "c"((uint32_t)msg)
    );
    return ret;
}

int32_t sys_register_service(const char* name) {
    int32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_REGISTER_SVC), "b"((uint32_t)name)
    );
    return ret;
}

uint32_t sys_lookup_service(const char* name) {
    uint32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_LOOKUP_SVC), "b"((uint32_t)name)
    );
    return ret;
}

/* Legacy wrappers */
int32_t sys_write(const char* buf, uint32_t len) {
    int32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_WRITE), "b"((uint32_t)buf), "c"(len)
    );
    return ret;
}

int32_t sys_read(char* buf, uint32_t len) {
    int32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_READ), "b"((uint32_t)buf), "c"(len)
    );
    return ret;
}

int32_t sys_getpid(void) {
    int32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_GETPID));
    return ret;
}

void sys_exit(int code) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_EXIT), "b"(code));
}

void sys_sleep(uint32_t ms) {
    __asm__ volatile ("int $0x80" : : "a"(SYS_SLEEP), "b"(ms));
}

uint32_t sys_get_ticks(void) {
    uint32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_GET_TICKS));
    return ret;
}

