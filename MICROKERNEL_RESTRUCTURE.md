# MicroKernel Architecture Restructuring Guide

## What You Have Now (Monolithic)

Everything runs in **ring 0** (kernel mode) in a **single address space**:

```
┌─────────────────────────────────────────────┐
│                  RING 0                      │
│                                              │
│  kernel.c  ─  boot, init everything         │
│  task.c    ─  scheduler (preemptive)         │
│  ipc.c     ─  message passing (unused)       │
│  paging.c  ─  identity-mapped, single PD     │
│  heap.c    ─  single kernel heap             │
│  syscall.c ─  int 0x80 (but stays in ring 0) │
│                                              │
│  ata.c     ─  disk driver                    │
│  fat16.c   ─  filesystem                     │
│  ntfs.c    ─  filesystem                     │
│  net.c     ─  RTL8139 + TCP/IP               │
│  keyboard.c─  keyboard driver                │
│  vga.c     ─  display driver                 │
│  shell.c   ─  user interface                 │
│  ramfs.c   ─  RAM filesystem + VFS dispatch  │
│  ... everything else ...                     │
└─────────────────────────────────────────────┘
```

**Specific problems:**
- `task.c` line 68: `*(--sp) = 0x08` — CS=0x08 is kernel code segment. All tasks run in ring 0.
- `paging.c`: Single page directory, identity-mapped. No per-process address spaces.
- `syscall.c`: Wrappers use `int 0x80` but there's no privilege transition — caller is already ring 0.
- `ipc.c`: Message passing exists but nothing uses it for actual service communication. Drivers call each other directly.
- `shell.c` at 52KB directly calls `fat16_write_file()`, `ntfs_read_file()`, `ata_print_info()` — tight coupling to every driver.

## What a Proper Microkernel Looks Like

```
┌────────────────────────────────────┐
│           RING 0 (Kernel)          │
│                                    │
│  Scheduler   (task switching)      │
│  IPC         (message passing)     │
│  VMM         (page tables, faults) │
│  IRQ Router  (dispatch to servers) │
│  Syscall     (entry/exit ring 3)   │
│                                    │
│  ~2000 lines total                 │
└────────────────────────────────────┘
         ↕ IPC messages ↕
┌──────────┐ ┌──────────┐ ┌──────────┐
│  VFS     │ │  Disk    │ │  Net     │  RING 3
│  Server  │ │  Driver  │ │  Driver  │  (user mode)
│          │ │  (ATA)   │ │  (RTL)   │
└──────────┘ └──────────┘ └──────────┘
┌──────────┐ ┌──────────┐ ┌──────────┐
│  FAT16   │ │  NTFS    │ │  Console │  RING 3
│  Server  │ │  Server  │ │  Server  │
└──────────┘ └──────────┘ └──────────┘
┌──────────────────────────────────────┐
│              Shell (user process)     │  RING 3
└──────────────────────────────────────┘
```

The kernel becomes tiny. Drivers run as **unprivileged processes** that talk to the kernel and each other through IPC messages. If the network driver crashes, the kernel is fine — just restart the driver.

### How a File Read Works (Microkernel Way)

```
Shell                VFS Server         FAT16 Server       ATA Driver
  │                      │                   │                  │
  │─── READ /disk/f.txt ─→                   │                  │
  │                      │── READ cluster 5 ─→                  │
  │                      │                   │── READ LBA 100 ──→
  │                      │                   │                  │── port I/O ──→ disk
  │                      │                   │                  │←── data ───────
  │                      │                   │←── sector data ──│
  │                      │←── file data ─────│                  │
  │←── file contents ────│                   │                  │
```

Every arrow is an IPC message. Slow? Yes, a bit. But isolated, debuggable, and restartable.

## Restructuring Plan (5 Phases)

Don't try to do this all at once. Each phase produces a working system.

---

### Phase 1: Per-Process Address Spaces

**What changes:** `paging.c`, `task.c`, `pmm.c`

Right now you have one page directory for everything. You need each task to get its own page directory so user processes can't see kernel memory.

**Memory layout per process:**
```
0x00000000 - 0x003FFFFF   Kernel (4MB, mapped in every process)
0x00400000 - 0x7FFFFFFF   User space (per-process)
0x80000000 - 0xFFFFFFFF   Kernel heap / driver mappings (mapped in every process)
```

**Changes to paging.c:**
```c
/* Create a new address space (page directory) */
uint32_t* paging_create_address_space(void) {
    /* Allocate a page-aligned frame for the new page directory */
    uint32_t pd_phys = pmm_alloc_frame();
    uint32_t* pd = (uint32_t*)pd_phys;  /* identity-mapped, so phys=virt for kernel */

    memset(pd, 0, 4096);

    /* Clone kernel mappings (first 4MB + upper half) into new PD.
     * This means every process can see the kernel, but we'll mark
     * those pages as supervisor-only so ring 3 can't access them. */
    for (int i = 0; i < 1024; i++) {
        if (kernel_page_directory[i] & PAGE_PRESENT) {
            /* Copy entry but clear PAGE_USER — kernel pages only */
            pd[i] = kernel_page_directory[i] & ~PAGE_USER;
        }
    }
    return pd;
}

/* Switch to a process's address space */
void paging_switch(uint32_t* pd) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint32_t)pd) : "memory");
}

/* Map a page in a specific address space as user-accessible */
void paging_map_user(uint32_t* pd, uint32_t virt, uint32_t phys) {
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        /* Allocate a page table */
        uint32_t pt_phys = pmm_alloc_frame();
        memset((void*)pt_phys, 0, 4096);
        pd[pd_idx] = pt_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }

    uint32_t* pt = (uint32_t*)(pd[pd_idx] & 0xFFFFF000);
    pt[pt_idx] = (phys & 0xFFFFF000) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
}
```

**Changes to task.c:**
```c
typedef struct {
    /* ... existing fields ... */
    uint32_t* page_directory;    /* Per-process page directory */
    bool      is_kernel;         /* Kernel task or user process? */
} task_t;
```

**On context switch** (in the timer tick handler):
```c
void task_switch_to(task_t* next) {
    if (next->page_directory != current->page_directory) {
        paging_switch(next->page_directory);
    }
    /* ... restore registers, iret ... */
}
```

**Changes to pmm.c** — you need a real frame allocator:
```c
/* Bitmap-based physical frame allocator */
static uint32_t frame_bitmap[32768];  /* Tracks 32768*32 = 1M frames = 4GB */
static uint32_t total_frames;

uint32_t pmm_alloc_frame(void) {
    for (uint32_t i = 0; i < total_frames / 32; i++) {
        if (frame_bitmap[i] != 0xFFFFFFFF) {
            for (int b = 0; b < 32; b++) {
                if (!(frame_bitmap[i] & (1 << b))) {
                    frame_bitmap[i] |= (1 << b);
                    return (i * 32 + b) * 4096;
                }
            }
        }
    }
    return 0; /* Out of memory */
}

void pmm_free_frame(uint32_t phys) {
    uint32_t idx = (phys / 4096) / 32;
    uint32_t bit = (phys / 4096) % 32;
    frame_bitmap[idx] &= ~(1 << bit);
}
```

---

### Phase 2: Ring 3 Execution

**What changes:** `task.c`, `gdt.c`, `boot.asm`

Your GDT needs user-mode segments. You probably already have slots for them but tasks never use them.

**GDT layout needed:**
```
Entry 0: NULL
Entry 1: Kernel Code  (ring 0, CS=0x08)
Entry 2: Kernel Data  (ring 0, DS=0x10)
Entry 3: User Code    (ring 3, CS=0x1B)  ← 0x18 | 3 (RPL)
Entry 4: User Data    (ring 3, DS=0x23)  ← 0x20 | 3 (RPL)
Entry 5: TSS          (for ring transitions)
```

**TSS (Task State Segment)** — essential for ring 3→0 transitions:
```c
typedef struct __attribute__((packed)) {
    uint32_t prev_tss, esp0, ss0, esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt, trap, iomap_base;
} tss_t;

static tss_t tss;

void tss_init(void) {
    memset(&tss, 0, sizeof(tss));
    tss.ss0 = 0x10;          /* Kernel data segment */
    tss.esp0 = kernel_stack_top;  /* Kernel stack for this task */
    tss.iomap_base = sizeof(tss);

    /* Install TSS in GDT entry 5 */
    gdt_set_gate(5, (uint32_t)&tss, sizeof(tss) - 1, 0x89, 0x00);
    __asm__ volatile ("ltr %%ax" : : "a"(0x28));  /* 0x28 = entry 5 * 8 */
}

/* Called on every context switch to update the kernel stack pointer */
void tss_set_kernel_stack(uint32_t esp0) {
    tss.esp0 = esp0;
}
```

**Creating a ring 3 task** — the stack frame changes:
```c
static void setup_user_stack(task_t* t, uint32_t entry_point) {
    /* Kernel stack for this task (used during syscalls/interrupts) */
    t->kernel_stack = (uint32_t)kmalloc(4096) + 4096;

    /* User stack mapped in user address space */
    uint32_t user_stack_phys = pmm_alloc_frame();
    uint32_t user_stack_virt = 0x7FFFF000;  /* Top of user space */
    paging_map_user(t->page_directory, user_stack_virt, user_stack_phys);

    /* Build iret frame on KERNEL stack for first switch.
     * Ring 0 → Ring 3 iret needs 5 values (not 3): */
    uint32_t* sp = (uint32_t*)t->kernel_stack;

    *(--sp) = 0x23;                /* SS: user data segment */
    *(--sp) = user_stack_virt + 4096; /* ESP: top of user stack */
    *(--sp) = 0x202;               /* EFLAGS: IF set */
    *(--sp) = 0x1B;                /* CS: user code segment (ring 3) */
    *(--sp) = entry_point;         /* EIP: user entry point */

    /* Fake interrupt frame */
    *(--sp) = 0;                   /* err_code */
    *(--sp) = 0;                   /* int_no */

    /* pusha */
    for (int i = 0; i < 8; i++) *(--sp) = 0;

    /* Segment registers — user data */
    *(--sp) = 0x23;                /* DS */

    t->saved_esp = (uint32_t)sp;
}
```

**Key insight:** When a ring 3 process does `int 0x80`, the CPU automatically:
1. Loads SS:ESP from the TSS (switches to kernel stack)
2. Pushes user SS, user ESP, EFLAGS, user CS, user EIP
3. Jumps to the interrupt handler

So on every context switch you must call `tss_set_kernel_stack()` with the current task's kernel stack.

---

### Phase 3: IPC as the Communication Backbone

**What changes:** `ipc.c` (major rewrite)

Your existing IPC has the right idea but needs to become **synchronous** (blocking send/receive) for a microkernel. The pattern is like Minix/L4:

```c
/* Syscall numbers for IPC */
#define SYS_SEND     20   /* Send message, block until received */
#define SYS_RECEIVE  21   /* Block until message arrives */
#define SYS_SENDREC  22   /* Send then wait for reply (most common) */
#define SYS_REPLY    23   /* Reply to a received message */
#define SYS_NOTIFY   24   /* Non-blocking notification (for IRQs) */

typedef struct {
    uint32_t sender;      /* Filled by kernel */
    uint32_t type;        /* Message type */
    union {
        uint8_t  raw[56];
        struct { uint32_t fd; uint32_t offset; uint32_t size; void* buf; } io;
        struct { uint32_t irq; } interrupt;
        struct { int32_t status; uint32_t value; } reply;
    };
} message_t;  /* 64 bytes, fits in cache line */
```

**Blocking semantics:**
```c
/* In syscall handler */
case SYS_SEND: {
    uint32_t dest_pid = regs->ebx;
    message_t* msg = (message_t*)regs->ecx;  /* In user space */

    task_t* dest = task_get(dest_pid);
    if (!dest) { regs->eax = -ESRCH; break; }

    if (dest->state == TASK_BLOCKED && dest->blocked_on == BLOCKED_RECEIVE) {
        /* Receiver is already waiting — deliver immediately */
        copy_message(msg, dest->msg_buf);
        dest->msg_buf->sender = current_task->id;
        dest->state = TASK_READY;
        regs->eax = 0;
    } else {
        /* Receiver not waiting — block sender */
        current_task->state = TASK_BLOCKED;
        current_task->blocked_on = BLOCKED_SEND;
        current_task->send_to = dest_pid;
        current_task->msg_buf = msg;
        task_schedule();  /* Switch away */
    }
    break;
}

case SYS_RECEIVE: {
    /* Check if anyone is already trying to send to us */
    task_t* sender = find_blocked_sender(current_task->id);
    if (sender) {
        message_t* user_buf = (message_t*)regs->ebx;
        copy_message(sender->msg_buf, user_buf);
        user_buf->sender = sender->id;
        sender->state = TASK_READY;
        regs->eax = 0;
    } else {
        /* Nobody sending — block until someone does */
        current_task->state = TASK_BLOCKED;
        current_task->blocked_on = BLOCKED_RECEIVE;
        current_task->msg_buf = (message_t*)regs->ebx;
        task_schedule();
    }
    break;
}
```

This is the **heart of a microkernel**. Everything else is built on send/receive.

---

### Phase 4: Move Drivers to Userspace Servers

This is where it gets real. Each driver becomes a standalone process that:
1. Starts in ring 3
2. Registers with the kernel to receive IRQs
3. Uses **I/O port permissions** (IOPL or I/O bitmap in TSS) to do hardware access
4. Communicates with other processes via IPC only

**ATA driver as a userspace server:**
```c
/* ata_server.c — runs as a ring 3 process */

/* Kernel grants us I/O port access via the TSS I/O bitmap */

void ata_server_main(void) {
    /* Register with the kernel: "I handle IRQ 14" */
    sys_register_irq(14);

    /* Register our service name so others can find us */
    sys_register_service("ata");

    message_t msg;
    while (1) {
        /* Block waiting for a request */
        sys_receive(&msg);

        switch (msg.type) {
        case ATA_READ_SECTOR: {
            uint32_t lba = msg.io.offset;
            void* buf = msg.io.buf;

            /* Do the actual I/O (we have port permission) */
            ata_do_pio_read(lba, local_buffer);

            /* Copy data to requester's buffer via shared memory
             * or a kernel "grant" mechanism */
            sys_grant_memory(msg.sender, local_buffer, 512);

            /* Reply */
            message_t reply = { .type = ATA_REPLY, .reply.status = 0 };
            sys_reply(msg.sender, &reply);
            break;
        }
        case IRQ_NOTIFICATION:
            /* Hardware interrupt happened, handle it */
            ata_handle_irq();
            break;
        }
    }
}
```

**I/O port access for userspace:**

Two approaches:
1. **IOPL=3** — gives the process access to ALL ports (simple but dangerous)
2. **TSS I/O bitmap** — per-port permission, more secure

```c
/* Simple approach: set IOPL in the task's EFLAGS */
void grant_io_access(task_t* t) {
    /* Set IOPL bits (12-13) to 3 in saved EFLAGS */
    /* This is on the kernel stack's iret frame */
    uint32_t* eflags = &t->saved_eflags;
    *eflags |= (3 << 12);  /* IOPL=3 */
}
```

**IRQ routing:**
```c
/* Kernel side: deliver hardware IRQs to registered servers */
static uint32_t irq_owners[16] = {0};  /* PID that handles each IRQ */

void irq_dispatch(uint32_t irq_num) {
    /* Acknowledge the PIC */
    pic_send_eoi(irq_num);

    uint32_t owner = irq_owners[irq_num];
    if (owner) {
        /* Send a notification message to the driver process */
        message_t notify = { .type = IRQ_NOTIFICATION, .interrupt.irq = irq_num };
        ipc_notify(owner, &notify);  /* Non-blocking */
    }
}
```

---

### Phase 5: VFS Server and Full Service Architecture

The final piece — a **VFS (Virtual File System)** server that routes file operations:

```
Shell: "read /disk/file.txt"
  │
  ├── send(VFS_PID, {READ, "/disk/file.txt"})
  │
VFS Server:
  ├── "/disk" prefix → FAT16 server owns this
  ├── send(FAT16_PID, {READ, "/file.txt"})
  │
FAT16 Server:
  ├── Need sector 500 from disk
  ├── send(ATA_PID, {READ_SECTOR, lba=500})
  │
ATA Server:
  ├── outb(0x1F7, READ_PIO) ...
  ├── reply(FAT16_PID, {sector_data})
  │
FAT16 Server:
  ├── reply(VFS_PID, {file_data})
  │
VFS Server:
  ├── reply(SHELL_PID, {file_contents})
```

---

## What Stays in the Kernel vs. What Moves Out

```
STAYS IN KERNEL (ring 0):             MOVES TO USERSPACE (ring 3):
─────────────────────────             ────────────────────────────
boot.asm    — bootstrap               ata.c       → ata_server
gdt.c       — segment descriptors     fat16.c     → fat16_server
idt.c       — interrupt table         ntfs.c      → ntfs_server
timer.c     — PIT tick (route IRQ0)   net.c       → net_server
task.c      — scheduler               keyboard.c  → kb_server
ipc.c       — message passing         vga.c       → console_server
paging.c    — VMM                     ramfs.c     → vfs_server
pmm.c       — frame allocator         shell.c     → shell (user process)
syscall.c   — trap handler            editor.c    → user process
                                       games.c     → user process
                                       image.c     → user process
```

**Kernel shrinks from ~10,000+ lines to ~2,000 lines.**

---

## Suggested File Restructure

```
microkernel/
├── kernel/                  ← Ring 0 only
│   ├── boot.asm
│   ├── gdt.c
│   ├── idt.c
│   ├── ipc.c               ← Rewritten: synchronous send/receive
│   ├── main.c              ← Minimal: init hardware, launch servers
│   ├── paging.c            ← Rewritten: per-process address spaces
│   ├── pmm.c               ← Rewritten: bitmap frame allocator
│   ├── syscall.c           ← Rewritten: IPC + memory grant syscalls
│   ├── task.c              ← Updated: ring 3 support, TSS
│   └── timer.c             ← Just tick counting + IRQ route
│
├── servers/                 ← Ring 3 privileged processes
│   ├── ata_server.c
│   ├── console_server.c    ← VGA + keyboard
│   ├── fat16_server.c
│   ├── ntfs_server.c
│   ├── net_server.c
│   └── vfs_server.c        ← Routes /disk, /proc, /home
│
├── user/                    ← Ring 3 normal processes
│   ├── shell.c
│   ├── editor.c
│   ├── games.c
│   └── init.c              ← First user process, launches servers
│
├── lib/                     ← Shared userspace library
│   ├── libc.c              ← printf, malloc, string functions
│   ├── ipc_client.c        ← sys_send, sys_receive wrappers
│   └── fs_client.c         ← open(), read(), write() via IPC
│
└── include/
    ├── kernel/              ← Kernel-internal headers
    └── user/                ← User-visible API
```

---

## Do It Incrementally

**Don't rewrite everything.** Here's the order that keeps your system working at every step:

1. **Frame allocator** — replace your PMM with a real bitmap allocator. Test: allocate and free 1000 frames.

2. **Per-process page directories** — create/destroy address spaces. Test: map a page in a new PD, switch to it, read the page, switch back.

3. **Ring 3 tasks** — add TSS, user GDT entries, ring 3 task creation. Test: launch a ring 3 task that does `int 0x80` to print "Hello from ring 3".

4. **Synchronous IPC** — rewrite IPC with blocking send/receive. Test: two tasks ping-pong messages.

5. **Move keyboard driver** — simplest driver. Make it a ring 3 server that receives IRQ1 notifications and sends keystrokes via IPC. Test: typing still works.

6. **Move VGA** — console server. Test: kprintf routes through IPC to console.

7. **Move ATA** — disk driver server. Test: `ls /disk` still works.

8. **Move FAT16/NTFS** — filesystem servers. Test: `save` and `load` still work.

9. **Move shell** — becomes a regular user process. Test: everything works.

Each step is independently testable. If step 5 breaks, you still have steps 1-4 working.

---

## Comparison: You vs. Minix vs. L4

```
                   Your OS      Minix 3       L4/seL4
                   ───────      ───────       ───────
Kernel lines       10,000+      ~6,000        ~10,000
Kernel mode        Everything   Sched+IPC     Sched+IPC+VMM
Drivers            Ring 0       Ring 3         Ring 3
IPC model          Async queues Sync rendzv    Sync + shared mem
Address spaces     1 (shared)   Per-process    Per-process
Protection         None         Full           Formal proof
File systems       In kernel    User servers   User servers
```

You're closest to early Linux (monolithic) right now. After this restructure you'd be closer to Minix 3, which is exactly what "microkernel" means.
