#include "timer.h"
#include "idt.h"
#include "task.h"
#include "vga.h"

static volatile uint32_t tick_count = 0;
static uint32_t timer_freq = 100;

static void timer_callback(registers_t* regs) {
    tick_count++;
    /* Drive preemptive scheduling */
    task_timer_tick(regs);
}

uint32_t timer_get_ticks(void) { 
    return tick_count; 
}

uint32_t timer_get_seconds(void) { 
    return tick_count / timer_freq; 
}

uint32_t timer_get_frequency(void) { 
    return timer_freq; 
}

void timer_get_uptime(uint32_t* hours, uint32_t* mins, uint32_t* secs) {
    uint32_t total = timer_get_seconds();
    *hours = total / 3600;
    *mins  = (total % 3600) / 60;
    *secs  = total % 60;
}

void timer_sleep(uint32_t ms) {
    uint32_t target = tick_count + (ms * timer_freq) / 1000;
    while (tick_count < target)
        hlt();
}

void timer_init(uint32_t frequency) {
    timer_freq = frequency;
    register_interrupt_handler(32, timer_callback);
    uint32_t divisor = 1193180 / frequency;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}