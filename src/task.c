#include "task.h"
#include "heap.h"
#include "timer.h"
#include "vga.h"
#include "gdt.h"
#include "paging.h"
#include "ipc.h"
#include "serial.h"

static task_t tasks[MAX_TASKS];
static int32_t current_task = -1;
static uint32_t next_pid = 0;
static bool scheduler_enabled = false;
static bool preemptive_enabled = true;
static uint32_t default_quantum = DEFAULT_QUANTUM;
static uint32_t sched_lock_count = 0;

/* Set by task_timer_tick, consumed by task_preempt_check */
static volatile uint32_t switch_new_esp = 0;
static volatile bool     switch_pending = false;
static uint32_t total_ctx_switches = 0;

void task_init(void) {
    memset(tasks, 0, sizeof(tasks));

    /* Create the kernel/idle task (PID 0) — runs in ring 0 */
    tasks[0].id = next_pid++;
    strcpy(tasks[0].name, "kernel");
    tasks[0].state = TASK_RUNNING;
    tasks[0].active = true;
    tasks[0].priority = 10;
    tasks[0].stack_base = 0;
    tasks[0].stack_size = 0;
    tasks[0].quantum = default_quantum;
    tasks[0].ticks_left = default_quantum;
    tasks[0].saved_esp = 0;
    tasks[0].total_ticks = 0;
    tasks[0].switches = 0;
    tasks[0].page_directory = NULL;  /* Uses kernel PD */
    tasks[0].is_user = false;
    tasks[0].kernel_stack_base = 0;
    tasks[0].kernel_stack_top = 0;
    tasks[0].blocked_on = BLOCKED_NONE;
    tasks[0].io_privileged = true;
    current_task = 0;
    scheduler_enabled = true;
}

/* Set up a fake interrupt frame for a RING 0 task (kernel task).
 * Same-privilege iret: only pops EIP, CS, EFLAGS (3 values). */
static void setup_kernel_stack(task_t* t, task_entry_t entry) {
    uint32_t stack_top = t->stack_base + t->stack_size;
    uint32_t* sp = (uint32_t*)stack_top;

    /* iret frame (kernel-to-kernel: 3 values) */
    *(--sp) = 0x202;           /* eflags: IF set */
    *(--sp) = SEG_KCODE;       /* cs: kernel code segment */
    *(--sp) = (uint32_t)entry; /* eip: task entry point */

    /* Fake int_no and err_code */
    *(--sp) = 0;               /* err_code */
    *(--sp) = 32;              /* int_no */

    /* pusha registers */
    *(--sp) = 0;               /* eax */
    *(--sp) = 0;               /* ecx */
    *(--sp) = 0;               /* edx */
    *(--sp) = 0;               /* ebx */
    *(--sp) = 0;               /* esp (ignored by popa) */
    *(--sp) = 0;               /* ebp */
    *(--sp) = 0;               /* esi */
    *(--sp) = 0;               /* edi */

    /* ds */
    *(--sp) = SEG_KDATA;       /* kernel data segment */

    t->saved_esp = (uint32_t)sp;
    t->regs.esp = (uint32_t)sp;
    t->regs.ebp = stack_top;
    t->regs.eip = (uint32_t)entry;
    t->regs.eflags = 0x202;
}

/* Set up a fake interrupt frame for a RING 3 task (user-mode server or process).
 * Cross-privilege iret: pops EIP, CS, EFLAGS, ESP, SS (5 values).
 * The frame is built on the task's KERNEL stack. */
static void setup_user_stack(task_t* t, task_entry_t entry) {
    /* Build the initial frame on the kernel stack */
    uint32_t* sp = (uint32_t*)t->kernel_stack_top;

    /* Cross-ring iret frame (5 values) */
    *(--sp) = SEG_UDATA;                   /* SS: user data segment */
    *(--sp) = t->stack_base + t->stack_size; /* ESP: top of user stack */
    *(--sp) = 0x202;                        /* EFLAGS: IF set */

    /* If this is an I/O privileged server, set IOPL=3 in EFLAGS */
    if (t->io_privileged) {
        sp[0] |= (3 << 12);  /* IOPL=3 — allows ring 3 code to use in/out */
    }

    *(--sp) = SEG_UCODE;                   /* CS: user code segment (ring 3) */
    *(--sp) = (uint32_t)entry;              /* EIP: user entry point */

    /* Fake int_no and err_code */
    *(--sp) = 0;                            /* err_code */
    *(--sp) = 32;                           /* int_no */

    /* pusha registers (eax, ecx, edx, ebx, esp_dummy, ebp, esi, edi) */
    *(--sp) = 0;                            /* eax */
    *(--sp) = 0;                            /* ecx */
    *(--sp) = 0;                            /* edx */
    *(--sp) = 0;                            /* ebx */
    *(--sp) = 0;                            /* esp (ignored by popa) */
    *(--sp) = 0;                            /* ebp */
    *(--sp) = 0;                            /* esi */
    *(--sp) = 0;                            /* edi */

    /* Segment register — user data */
    *(--sp) = SEG_UDATA;                   /* DS: user data segment */

    t->saved_esp = (uint32_t)sp;
}

int32_t task_create(const char* name, task_entry_t entry, uint32_t priority) {
    int slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (!tasks[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    void* stack = kmalloc(TASK_STACK_SIZE);
    if (!stack) return -1;

    task_t* t = &tasks[slot];
    memset(t, 0, sizeof(task_t));
    t->id = next_pid++;
    strncpy(t->name, name, TASK_NAME_LEN - 1);
    t->name[TASK_NAME_LEN - 1] = '\0';
    t->state = TASK_READY;
    t->active = true;
    t->priority = priority;
    t->stack_base = (uint32_t)stack;
    t->stack_size = TASK_STACK_SIZE;
    t->quantum = default_quantum;
    t->ticks_left = default_quantum;
    t->is_user = false;
    t->page_directory = NULL; /* Kernel PD */
    t->blocked_on = BLOCKED_NONE;

    setup_kernel_stack(t, entry);
    return t->id;
}

int32_t task_create_user(const char* name, task_entry_t entry,
                         uint32_t priority, bool io_privileged) {
    int slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (!tasks[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    /* Allocate user stack */
    void* user_stack = kmalloc(TASK_STACK_SIZE);
    if (!user_stack) return -1;

    /* Allocate kernel stack (for ring 3 → ring 0 transitions) */
    void* kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!kernel_stack) { kfree(user_stack); return -1; }

    /* Create per-process address space */
    uint32_t* pd = paging_create_address_space();
    if (!pd) { kfree(user_stack); kfree(kernel_stack); return -1; }

    task_t* t = &tasks[slot];
    memset(t, 0, sizeof(task_t));
    t->id = next_pid++;
    strncpy(t->name, name, TASK_NAME_LEN - 1);
    t->name[TASK_NAME_LEN - 1] = '\0';
    t->state = TASK_READY;
    t->active = true;
    t->priority = priority;
    t->stack_base = (uint32_t)user_stack;
    t->stack_size = TASK_STACK_SIZE;
    t->quantum = default_quantum;
    t->ticks_left = default_quantum;
    t->is_user = true;
    t->io_privileged = io_privileged;
    t->page_directory = pd;
    t->kernel_stack_base = (uint32_t)kernel_stack;
    t->kernel_stack_top = (uint32_t)kernel_stack + KERNEL_STACK_SIZE;
    t->blocked_on = BLOCKED_NONE;

    setup_user_stack(t, entry);
    return t->id;
}

int32_t task_create_from_elf(const char* name, uint32_t entry_point,
                             uint32_t user_stack_top, uint32_t* page_dir,
                             uint32_t kstack_base, uint32_t kstack_size,
                             uint32_t priority, bool io_privileged) {
    /*
     * Create a task from an ELF-loaded binary with a pre-built
     * isolated address space. Unlike task_create_user(), the address
     * space and stacks are already allocated by the ELF loader.
     *
     * The key difference: this task's page directory has kernel pages
     * as SUPERVISOR-ONLY. The task truly cannot touch kernel memory.
     */
    int slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (!tasks[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    task_t* t = &tasks[slot];
    memset(t, 0, sizeof(task_t));
    t->id = next_pid++;
    strncpy(t->name, name, TASK_NAME_LEN - 1);
    t->name[TASK_NAME_LEN - 1] = '\0';
    t->state = TASK_READY;
    t->active = true;
    t->priority = priority;
    t->stack_base = user_stack_top - (4 * 4096); /* USER_STACK_PAGES pages below top */
    t->stack_size = 4 * 4096;
    t->quantum = default_quantum;
    t->ticks_left = default_quantum;
    t->is_user = true;
    t->io_privileged = io_privileged;
    t->page_directory = page_dir;
    t->kernel_stack_base = kstack_base;
    t->kernel_stack_top = kstack_base + kstack_size;
    t->blocked_on = BLOCKED_NONE;

    /* Build the initial iret frame on the kernel stack.
     * Same as setup_user_stack, but entry_point is a raw address
     * (from ELF header) rather than a function pointer. */
    uint32_t* sp = (uint32_t*)t->kernel_stack_top;

    /* Cross-ring iret frame (5 values) */
    *(--sp) = SEG_UDATA;               /* SS: user data segment */
    *(--sp) = user_stack_top;           /* ESP: user stack top */
    *(--sp) = 0x202;                    /* EFLAGS: IF set */
    if (io_privileged) {
        sp[0] |= (3 << 12);            /* IOPL=3 for hardware access */
    }
    *(--sp) = SEG_UCODE;               /* CS: user code (ring 3) */
    *(--sp) = entry_point;              /* EIP: ELF entry point */

    /* Fake int_no and err_code */
    *(--sp) = 0;                        /* err_code */
    *(--sp) = 32;                       /* int_no */

    /* pusha registers (all zero) */
    *(--sp) = 0;                        /* eax */
    *(--sp) = 0;                        /* ecx */
    *(--sp) = 0;                        /* edx */
    *(--sp) = 0;                        /* ebx */
    *(--sp) = 0;                        /* esp (ignored by popa) */
    *(--sp) = 0;                        /* ebp */
    *(--sp) = 0;                        /* esi */
    *(--sp) = 0;                        /* edi */

    /* Segment register — user data */
    *(--sp) = SEG_UDATA;               /* DS: user data segment */

    t->saved_esp = (uint32_t)sp;

    return t->id;
}

static int find_next_task(void) {
    uint32_t now = timer_get_ticks();

    /* Wake sleeping tasks */
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].active && tasks[i].state == TASK_SLEEPING) {
            if (now >= tasks[i].wake_tick)
                tasks[i].state = TASK_READY;
        }
    }

    /* Round-robin with priority — skip blocked tasks.
     * CRITICAL: Skip the current task in the first pass so that
     * task_sleep()/task_yield() actually give other tasks CPU time.
     * Without this, a high-priority task that sleeps for 1 tick gets
     * woken above and immediately re-selected, starving lower-priority
     * tasks forever. */
    int best = -1;
    uint32_t best_prio = 0xFFFFFFFF;
    int start = (current_task + 1) % MAX_TASKS;

    for (int n = 0; n < MAX_TASKS; n++) {
        int i = (start + n) % MAX_TASKS;
        if (i == current_task) continue;  /* give others a chance first */
        if (tasks[i].active && (tasks[i].state == TASK_READY || tasks[i].state == TASK_RUNNING)) {
            if (tasks[i].priority < best_prio) {
                best = i;
                best_prio = tasks[i].priority;
            }
        }
    }

    /* Fall back to current task only if no other task is runnable */
    if (best < 0) {
        if (tasks[current_task].active &&
            (tasks[current_task].state == TASK_READY || tasks[current_task].state == TASK_RUNNING))
            return current_task;
        return 0;
    }
    return best;
}

/* Perform a context switch: update TSS and switch page directory */
static void do_switch_context(int next) {
    task_t* next_task = &tasks[next];

    /* Update TSS kernel stack for the new task.
     * When the CPU takes an interrupt while this ring 3 task is running,
     * it loads SS:ESP from the TSS to switch to the kernel stack. */
    if (next_task->is_user && next_task->kernel_stack_top) {
        tss_set_kernel_stack(next_task->kernel_stack_top);
    }

    /* Switch page directory if different from current */
    uint32_t* new_pd = next_task->page_directory;
    uint32_t* cur_pd = tasks[current_task].page_directory;

    if (!new_pd) new_pd = paging_get_kernel_pd();
    if (!cur_pd) cur_pd = paging_get_kernel_pd();

    if (new_pd != cur_pd) {
        paging_switch(new_pd);
    }
}

void task_timer_tick(registers_t* regs) {
    if (!scheduler_enabled || current_task < 0 || sched_lock_count > 0)
        return;

    tasks[current_task].cpu_ticks++;
    tasks[current_task].total_ticks++;

    if (!preemptive_enabled)
        return;

    /* Always try to switch if current task is sleeping/blocked/terminated.
     * Without this, a task that sleeps mid-quantum would waste ticks
     * counting down without switching, and sleeping tasks wouldn't get
     * woken because find_next_task never runs. */
    bool must_switch = (tasks[current_task].state == TASK_SLEEPING ||
                        tasks[current_task].state == TASK_BLOCKED ||
                        tasks[current_task].state == TASK_TERMINATED);

    if (!must_switch) {
        if (tasks[current_task].ticks_left > 0)
            tasks[current_task].ticks_left--;

        if (tasks[current_task].ticks_left > 0)
            return;  /* Still has quantum, keep running */
    }

    /* Quantum expired or task is sleeping/blocked — find next task */
    int next = find_next_task();
    if (next == current_task) {
        tasks[current_task].ticks_left = tasks[current_task].quantum;
        return;
    }

    /* Save current task's interrupt frame */
    tasks[current_task].saved_esp = (uint32_t)regs;
    if (tasks[current_task].state == TASK_RUNNING)
        tasks[current_task].state = TASK_READY;

    /* Switch to next task */
    tasks[next].state = TASK_RUNNING;
    tasks[next].ticks_left = tasks[next].quantum;
    tasks[next].switches++;
    total_ctx_switches++;

    /* Update TSS and page directory for the new task */
    do_switch_context(next);

    int prev = current_task;
    current_task = next;
    (void)prev;

    switch_new_esp = tasks[next].saved_esp;
    switch_pending = true;
}

uint32_t task_preempt_check(registers_t* regs) {
    (void)regs;
    if (switch_pending) {
        switch_pending = false;
        uint32_t esp = switch_new_esp;
        switch_new_esp = 0;
        return esp;
    }
    return 0;
}

/* Cooperative context switch (kept for voluntary yields) */
static inline void switch_context(task_regs_t* old_regs, task_regs_t* new_regs) {
    __asm__ volatile (
        "movl %%esp, %0\n"
        "movl %%ebp, %1\n"
        "movl %%ebx, %2\n"
        "movl %%esi, %3\n"
        "movl %%edi, %4\n"
        : "=m"(old_regs->esp), "=m"(old_regs->ebp),
          "=m"(old_regs->ebx), "=m"(old_regs->esi), "=m"(old_regs->edi)
    );

    __asm__ volatile (
        "movl %0, %%esp\n"
        "movl %1, %%ebp\n"
        "movl %2, %%ebx\n"
        "movl %3, %%esi\n"
        "movl %4, %%edi\n"
        :
        : "m"(new_regs->esp), "m"(new_regs->ebp),
          "m"(new_regs->ebx), "m"(new_regs->esi), "m"(new_regs->edi)
    );
}

void task_schedule(void) {
    if (!scheduler_enabled || current_task < 0) return;

    int next = find_next_task();
    if (next == current_task) {
        tasks[current_task].cpu_ticks++;
        return;
    }

    task_t* old_task = &tasks[current_task];
    task_t* new_task = &tasks[next];

    if (old_task->state == TASK_RUNNING)
        old_task->state = TASK_READY;

    new_task->state = TASK_RUNNING;
    new_task->cpu_ticks++;
    new_task->ticks_left = new_task->quantum;
    new_task->switches++;
    total_ctx_switches++;

    /* Update TSS and page directory */
    do_switch_context(next);

    int prev = current_task;
    current_task = next;
    (void)prev;

    switch_context(&old_task->regs, &new_task->regs);
}

void task_yield(void) {
    if (current_task < 0) return;
    tasks[current_task].ticks_left = 0;

    /* Force all context switches through the preemptive path (timer IRQ).
     *
     * The cooperative switch_context() saves/restores only 5 registers via
     * task_regs_t, but setup_user_stack() builds a full interrupt frame and
     * only sets saved_esp. Tasks that have never been cooperatively switched
     * have regs.esp=0, so switch_context would load ESP=0 and crash.
     *
     * By spinning with interrupts enabled, the timer IRQ fires and the
     * preemptive scheduler in irq_common_stub correctly restores the full
     * interrupt frame for both ring 0 and ring 3 tasks. */
    sti();
    if (tasks[current_task].state == TASK_BLOCKED ||
        tasks[current_task].state == TASK_SLEEPING ||
        tasks[current_task].state == TASK_TERMINATED) {
        while (tasks[current_task].state != TASK_READY &&
               tasks[current_task].state != TASK_RUNNING) {
            hlt();
        }
    } else {
        hlt();
    }
}

void task_sleep(uint32_t ms) {
    if (current_task < 0) return;
    tasks[current_task].state = TASK_SLEEPING;
    tasks[current_task].wake_tick = timer_get_ticks() + (ms * 100) / 1000;
    task_yield();
}

void task_exit(void) {
    if (current_task <= 0) return;
    task_t* t = &tasks[current_task];
    t->state = TASK_TERMINATED;
    t->active = false;

    serial_printf("task_exit: PID %u '%s' is_user=%d stack_base=%x kstack_base=%x pd=%x\n",
                  t->id, t->name, t->is_user, t->stack_base,
                  t->kernel_stack_base, (uint32_t)t->page_directory);

    /* Free user stack — but ONLY if it's a kmalloc'd pointer (kernel heap).
     * ELF tasks have user stacks allocated via pmm_alloc_page() and mapped
     * into user address space; those are freed by paging_destroy_address_space(). */
    if (t->stack_base && t->stack_base < 0x40000000) {
        serial_printf("task_exit: kfree stack_base %x\n", t->stack_base);
        kfree((void*)t->stack_base);
    } else if (t->stack_base) {
        serial_printf("task_exit: SKIP kfree stack_base %x (user-space VA, freed with PD)\n",
                      t->stack_base);
    }

    /* Free kernel stack (ring 3 tasks only) */
    if (t->kernel_stack_base) {
        serial_printf("task_exit: kfree kernel_stack %x\n", t->kernel_stack_base);
        kfree((void*)t->kernel_stack_base);
    }

    /* Free page directory (also frees user-space page tables and frames).
     * CRITICAL: Switch to kernel PD first since we're currently using
     * the task's PD during this syscall! */
    if (t->page_directory) {
        serial_printf("task_exit: switching to kernel PD before destroy\n");
        paging_switch(paging_get_kernel_pd());
        serial_printf("task_exit: destroying address space %x\n",
                      (uint32_t)t->page_directory);
        paging_destroy_address_space(t->page_directory);
        t->page_directory = NULL;
    }

    task_yield();
    for(;;) hlt();
}

void task_kill(uint32_t id) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].active && tasks[i].id == id && i != 0) {
            serial_printf("task_kill: PID %u '%s' is_user=%d stack_base=%x kstack=%x pd=%x\n",
                          tasks[i].id, tasks[i].name, tasks[i].is_user,
                          tasks[i].stack_base, tasks[i].kernel_stack_base,
                          (uint32_t)tasks[i].page_directory);
            tasks[i].state = TASK_TERMINATED;
            tasks[i].active = false;
            /* Only kfree stack if it's a kernel heap allocation (below user base) */
            if (tasks[i].stack_base && tasks[i].stack_base < 0x40000000)
                kfree((void*)tasks[i].stack_base);
            else if (tasks[i].stack_base)
                serial_printf("task_kill: SKIP kfree stack %x (user VA)\n",
                              tasks[i].stack_base);
            if (tasks[i].kernel_stack_base)
                kfree((void*)tasks[i].kernel_stack_base);
            if (tasks[i].page_directory) {
                /* Switch to kernel PD if we're currently using this task's PD */
                uint32_t cr3;
                __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
                if (cr3 == (uint32_t)tasks[i].page_directory) {
                    serial_printf("task_kill: switching away from task's PD before destroy\n");
                    paging_switch(paging_get_kernel_pd());
                }
                paging_destroy_address_space(tasks[i].page_directory);
                tasks[i].page_directory = NULL;
            }
            return;
        }
    }
}

task_t* task_get_current(void) {
    return (current_task >= 0) ? &tasks[current_task] : NULL;
}

task_t* task_get_by_pid(uint32_t pid) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].active && tasks[i].id == pid)
            return &tasks[i];
    }
    return NULL;
}

uint32_t task_count(void) {
    uint32_t count = 0;
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].active) count++;
    return count;
}

void task_set_quantum(uint32_t ticks) {
    if (ticks < 1) ticks = 1;
    if (ticks > 1000) ticks = 1000;
    default_quantum = ticks;
}

uint32_t task_get_quantum(void) { return default_quantum; }
bool task_is_preemptive(void) { return preemptive_enabled; }
void task_set_preemptive(bool enabled) { preemptive_enabled = enabled; }
void task_lock_scheduler(void)   { sched_lock_count++; }
void task_unlock_scheduler(void) { if (sched_lock_count > 0) sched_lock_count--; }
task_t* task_get_all(void) { return tasks; }
uint32_t task_total_switches(void) { return total_ctx_switches; }

static const char* state_names[] = {
    "READY", "RUNNING", "SLEEPING", "BLOCKED", "TERMINATED"
};

static const char* blocked_names[] = {
    "", "SEND", "RECV", "SENDREC"
};

void task_list(void) {
    kprintf("  PID  %-16s  STATE      RING  PRI  QUANTUM  CPU     CTX\n", "NAME");
    kprintf("  ---  %-16s  ---------  ----  ---  -------  ------  ------\n", "----");
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].active) {
            kprintf("  %-4u %-16s  ", tasks[i].id, tasks[i].name);
            if (tasks[i].state == TASK_BLOCKED && tasks[i].blocked_on > 0) {
                kprintf("BLK/%-4s  ", blocked_names[tasks[i].blocked_on]);
            } else {
                kprintf("%-9s  ", state_names[tasks[i].state]);
            }
            kprintf("%-4s  %-3u  %-7u  %-6u  %u\n",
                    tasks[i].is_user ? "3" : "0",
                    tasks[i].priority, tasks[i].quantum,
                    tasks[i].cpu_ticks, tasks[i].switches);
        }
    }
    kprintf("\n  Scheduler: %s | Quantum: %u ticks | Total switches: %u\n",
            preemptive_enabled ? "preemptive" : "cooperative",
            default_quantum, total_ctx_switches);
}
