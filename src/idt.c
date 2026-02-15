#include "idt.h"
#include "vga.h"
#include "ipc.h"
#include "timer.h"
#include "serial.h"
#include "task.h"

extern void idt_load(uint32_t);

static struct idt_entry idt_entries[256];
static struct idt_ptr   idt_pointer;
static isr_t interrupt_handlers[256];

static const char* exception_names[] = {
    "Division By Zero", "Debug", "NMI", "Breakpoint",
    "Overflow", "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Overrun", "Invalid TSS", "Segment Not Present",
    "Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
    "x87 FPU Error", "Alignment Check", "Machine Check", "SIMD FP Exception",
    "Virtualization", "Control Protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection", "VMM Communication", "Security Exception", "Reserved"
};

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt_entries[num].base_lo = base & 0xFFFF;
    idt_entries[num].base_hi = (base >> 16) & 0xFFFF;
    idt_entries[num].sel     = sel;
    idt_entries[num].always0 = 0;
    idt_entries[num].flags   = flags;
}

static void pic_remap(void) {
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();
    outb(0xA1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xF8);  /* Master: enable IRQ 0,1,2 only */
    outb(0xA1, 0xFF);  /* Slave: all masked */
}

void idt_init(void) {
    idt_pointer.limit = sizeof(struct idt_entry) * 256 - 1;
    idt_pointer.base  = (uint32_t)&idt_entries;

    memset(idt_entries, 0, sizeof(idt_entries));
    memset(interrupt_handlers, 0, sizeof(interrupt_handlers));

    pic_remap();

    /* ISRs 0-31 (CPU exceptions) */
    idt_set_gate(0,  (uint32_t)isr0,  0x08, 0x8E);
    idt_set_gate(1,  (uint32_t)isr1,  0x08, 0x8E);
    idt_set_gate(2,  (uint32_t)isr2,  0x08, 0x8E);
    idt_set_gate(3,  (uint32_t)isr3,  0x08, 0x8E);
    idt_set_gate(4,  (uint32_t)isr4,  0x08, 0x8E);
    idt_set_gate(5,  (uint32_t)isr5,  0x08, 0x8E);
    idt_set_gate(6,  (uint32_t)isr6,  0x08, 0x8E);
    idt_set_gate(7,  (uint32_t)isr7,  0x08, 0x8E);
    idt_set_gate(8,  (uint32_t)isr8,  0x08, 0x8E);
    idt_set_gate(9,  (uint32_t)isr9,  0x08, 0x8E);
    idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8E);
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E);
    idt_set_gate(15, (uint32_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint32_t)isr16, 0x08, 0x8E);
    idt_set_gate(17, (uint32_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint32_t)isr18, 0x08, 0x8E);
    idt_set_gate(19, (uint32_t)isr19, 0x08, 0x8E);
    idt_set_gate(20, (uint32_t)isr20, 0x08, 0x8E);
    idt_set_gate(21, (uint32_t)isr21, 0x08, 0x8E);
    idt_set_gate(22, (uint32_t)isr22, 0x08, 0x8E);
    idt_set_gate(23, (uint32_t)isr23, 0x08, 0x8E);
    idt_set_gate(24, (uint32_t)isr24, 0x08, 0x8E);
    idt_set_gate(25, (uint32_t)isr25, 0x08, 0x8E);
    idt_set_gate(26, (uint32_t)isr26, 0x08, 0x8E);
    idt_set_gate(27, (uint32_t)isr27, 0x08, 0x8E);
    idt_set_gate(28, (uint32_t)isr28, 0x08, 0x8E);
    idt_set_gate(29, (uint32_t)isr29, 0x08, 0x8E);
    idt_set_gate(30, (uint32_t)isr30, 0x08, 0x8E);
    idt_set_gate(31, (uint32_t)isr31, 0x08, 0x8E);

    /* IRQs 0-15 */
    idt_set_gate(32, (uint32_t)irq0,  0x08, 0x8E);
    idt_set_gate(33, (uint32_t)irq1,  0x08, 0x8E);
    idt_set_gate(34, (uint32_t)irq2,  0x08, 0x8E);
    idt_set_gate(35, (uint32_t)irq3,  0x08, 0x8E);
    idt_set_gate(36, (uint32_t)irq4,  0x08, 0x8E);
    idt_set_gate(37, (uint32_t)irq5,  0x08, 0x8E);
    idt_set_gate(38, (uint32_t)irq6,  0x08, 0x8E);
    idt_set_gate(39, (uint32_t)irq7,  0x08, 0x8E);
    idt_set_gate(40, (uint32_t)irq8,  0x08, 0x8E);
    idt_set_gate(41, (uint32_t)irq9,  0x08, 0x8E);
    idt_set_gate(42, (uint32_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint32_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);
    idt_set_gate(45, (uint32_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E);
    idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E);

    /* Syscall gate: INT 0x80 with DPL=3 so ring 3 code can invoke it */
    idt_set_gate(128, (uint32_t)isr128, 0x08, 0xEE);

    idt_load((uint32_t)&idt_pointer);
}

void register_interrupt_handler(uint8_t n, isr_t handler) {
    interrupt_handlers[n] = handler;
}

void isr_handler(registers_t* regs) {
    if (interrupt_handlers[regs->int_no]) {
        interrupt_handlers[regs->int_no](regs);
    } else {
        const char* name = (regs->int_no < 32) ? exception_names[regs->int_no] : "Unknown";
        serial_printf("\n!!! EXCEPTION: %s (int %u, err=%x, eip=%x)\n",
                      name, regs->int_no, regs->err_code, regs->eip);
        task_t* t = task_get_current();
        if (t) serial_printf("  Task: '%s' PID=%u\n", t->name, t->id);
        kprintf("\n PANIC: %s (int %u, err=%x, eip=%x)\n",
                name, regs->int_no, regs->err_code, regs->eip);
        kprintf("  System halted.\n");
        cli();
        for (;;) hlt();
    }
}

/* Returns new ESP for task switch, or 0 for no switch */
extern uint32_t task_preempt_check(registers_t* regs);

uint32_t irq_handler(registers_t* regs) {
    /* ACK the PIC */
    if (regs->int_no >= 40) outb(0xA0, 0x20);
    outb(0x20, 0x20);

    /* Call the kernel-registered handler (timer, keyboard, etc.) */
    if (interrupt_handlers[regs->int_no])
        interrupt_handlers[regs->int_no](regs);

    /* Route IRQ to userspace server via IPC notification.
     * This is how the microkernel delivers hardware interrupts
     * to driver servers running in ring 3. */
    uint32_t irq_num = regs->int_no - 32;
    if (irq_num < 16) {
        uint32_t owner = irq_get_owner(irq_num);
        if (owner) {
            message_t notify;
            memset(&notify, 0, sizeof(notify));
            notify.type = MSG_IRQ_NOTIFY;
            notify.interrupt.irq = irq_num;
            notify.interrupt.ticks = timer_get_ticks();
            ipc_notify(owner, &notify);
        }
    }

    return task_preempt_check(regs);
}

void irq_unmask(uint8_t irq) {
    uint16_t port;
    if (irq < 8) {
        port = 0x21;
    } else {
        port = 0xA1;
        irq -= 8;
    }
    uint8_t mask = inb(port);
    mask &= ~(1 << irq);
    outb(port, mask);
}
