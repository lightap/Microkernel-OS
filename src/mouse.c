#include "mouse.h"
#include "idt.h"
#include "virtio_input.h"
#include "serial.h"

#define MOUSE_PORT   0x60
#define MOUSE_CMD    0x64
#define MOUSE_STATUS 0x64

static volatile int mx = 160, my = 100;
static volatile int mx_max = 319, my_max = 199;
static volatile uint8_t mouse_buttons = 0;
static volatile uint8_t prev_buttons = 0;
static volatile uint8_t packet[3];
static volatile int      pkt_idx = 0;
static volatile bool ps2_received = false;   /* true once PS/2 gets any data */
static bool use_virtio_input = false;        /* true when virtio-input is active */

static void mouse_wait_write(void) {
    int timeout = 100000;
    while (timeout-- > 0) {
        if (!(inb(MOUSE_STATUS) & 0x02)) return;
    }
}

static void mouse_wait_read(void) {
    int timeout = 100000;
    while (timeout-- > 0) {
        if (inb(MOUSE_STATUS) & 0x01) return;
    }
}

static void mouse_cmd(uint8_t cmd) {
    mouse_wait_write();
    outb(MOUSE_CMD, 0xD4);   /* Send to mouse */
    mouse_wait_write();
    outb(MOUSE_PORT, cmd);
    mouse_wait_read();
    inb(MOUSE_PORT);          /* Read ACK */
}

static void mouse_callback(registers_t* regs) {
    (void)regs;
    uint8_t status = inb(MOUSE_STATUS);
    if (!(status & 0x20)) return;  /* Not a mouse byte */

    uint8_t data = inb(MOUSE_PORT);
    packet[pkt_idx] = data;
    pkt_idx++;

    if (pkt_idx == 1) {
        /* First byte: verify bit 3 is set (sync check) */
        if (!(data & 0x08)) { pkt_idx = 0; return; }
    }

    if (pkt_idx >= 3) {
        pkt_idx = 0;
        ps2_received = true;

        prev_buttons = mouse_buttons;
        mouse_buttons = packet[0] & 0x07;

        int dx = (int)packet[1] - ((packet[0] & 0x10) ? 256 : 0);
        int dy = (int)packet[2] - ((packet[0] & 0x20) ? 256 : 0);

        mx += dx;
        my -= dy;  /* Mouse Y is inverted */

        if (mx < 0) mx = 0;
        if (mx > mx_max) mx = mx_max;
        if (my < 0) my = 0;
        if (my > my_max) my = my_max;
    }
}

void mouse_init(void) {
    /* Enable auxiliary device (mouse) on PS/2 controller */
    mouse_wait_write();
    outb(MOUSE_CMD, 0xA8);

    /* Enable interrupt for mouse */
    mouse_wait_write();
    outb(MOUSE_CMD, 0x20);  /* Read controller config */
    mouse_wait_read();
    uint8_t config = inb(MOUSE_PORT);
    config |= 0x02;  /* Enable IRQ12 */
    config &= ~0x20;  /* Enable mouse clock */
    mouse_wait_write();
    outb(MOUSE_CMD, 0x60);  /* Write controller config */
    mouse_wait_write();
    outb(MOUSE_PORT, config);

    /* Reset mouse */
    mouse_cmd(0xFF);
    /* Wait for self-test result */
    mouse_wait_read(); inb(MOUSE_PORT); /* 0xAA */
    mouse_wait_read(); inb(MOUSE_PORT); /* 0x00 */

    /* Set defaults */
    mouse_cmd(0xF6);

    /* Set sample rate to 100 */
    mouse_cmd(0xF3);
    mouse_cmd(100);

    /* Set resolution */
    mouse_cmd(0xE8);
    mouse_cmd(0x02); /* 4 counts/mm */

    /* Enable data reporting */
    mouse_cmd(0xF4);

    /* Register IRQ12 handler and unmask */
    register_interrupt_handler(44, mouse_callback);
    irq_unmask(12);

    /* Also try to initialize virtio-input as a fallback */
    if (virtio_input_available()) {
        if (virtio_input_init()) {
            use_virtio_input = true;
            serial_printf("mouse: virtio-input available as fallback\n");
        }
    }
}

void mouse_poll(void) {
    if (use_virtio_input) {
        virtio_input_poll();
    }
}

int  mouse_get_x(void) {
    if (use_virtio_input && !ps2_received)
        return virtio_input_get_x();
    return mx;
}

int  mouse_get_y(void) {
    if (use_virtio_input && !ps2_received)
        return virtio_input_get_y();
    return my;
}

void mouse_set_bounds(int max_x, int max_y) {
    mx_max = max_x;
    my_max = max_y;
    if (mx > mx_max) mx = mx_max;
    if (my > my_max) my = my_max;
    if (use_virtio_input)
        virtio_input_set_bounds(max_x, max_y);
}

bool mouse_left_held(void) {
    if (use_virtio_input && !ps2_received)
        return virtio_input_left_held();
    return (mouse_buttons & 0x01) != 0;
}

bool mouse_right_held(void) {
    if (use_virtio_input && !ps2_received)
        return virtio_input_right_held();
    return (mouse_buttons & 0x02) != 0;
}

bool mouse_left_click(void) {
    if (use_virtio_input && !ps2_received)
        return virtio_input_left_click();
    return (mouse_buttons & 0x01) && !(prev_buttons & 0x01);
}

bool mouse_right_click(void) {
    if (use_virtio_input && !ps2_received)
        return virtio_input_right_click();
    return (mouse_buttons & 0x02) && !(prev_buttons & 0x02);
}

