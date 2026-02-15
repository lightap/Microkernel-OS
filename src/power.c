#include "power.h"
#include "vga.h"

void power_reboot(void) {
    kprintf("\nRebooting...\n");
    /* Pulse the keyboard controller reset line */
    uint8_t good = 0x02;
    while (good & 0x02)
        good = inb(0x64);
    outb(0x64, 0xFE);
    hlt();
}

void power_shutdown(void) {
    kprintf("\nShutting down...\n");
    /* QEMU/Bochs shutdown via ISA debug exit */
    outw(0x604, 0x2000);
    /* Try ACPI shutdown for QEMU */
    outw(0xB004, 0x2000);
    /* If that didn't work, just halt */
    kprintf("System halted. You can turn off your computer.\n");
    cli();
    for (;;) hlt();
}
