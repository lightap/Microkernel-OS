#ifndef VIRTIO_INPUT_H
#define VIRTIO_INPUT_H

#include "types.h"

/*
 * VirtIO Input Device Driver
 *
 * Supports virtio-tablet-pci and virtio-mouse-pci devices.
 * Used as a fallback when PS/2 mouse doesn't work (e.g. with
 * virtio-gpu-gl-pci + gtk,gl=on display).
 *
 * QEMU usage: add -device virtio-tablet-pci to command line
 */

/* Linux input event types */
#define EV_SYN  0x00
#define EV_KEY  0x01
#define EV_REL  0x02
#define EV_ABS  0x03

/* Relative axes */
#define REL_X   0x00
#define REL_Y   0x01

/* Absolute axes */
#define ABS_X   0x00
#define ABS_Y   0x01

/* Button codes */
#define BTN_LEFT    0x110
#define BTN_RIGHT   0x111
#define BTN_MIDDLE  0x112

/* SYN codes */
#define SYN_REPORT  0x00

/* VirtIO input config select values */
#define VIRTIO_INPUT_CFG_UNSET      0x00
#define VIRTIO_INPUT_CFG_ID_NAME    0x01
#define VIRTIO_INPUT_CFG_ID_SERIAL  0x02
#define VIRTIO_INPUT_CFG_ID_DEVIDS  0x03
#define VIRTIO_INPUT_CFG_PROP_BITS  0x10
#define VIRTIO_INPUT_CFG_EV_BITS    0x11
#define VIRTIO_INPUT_CFG_ABS_INFO   0x12

/* VirtIO input event (8 bytes, matches Linux input_event) */
typedef struct {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} __attribute__((packed)) virtio_input_event_t;

/* Absolute axis info */
typedef struct {
    uint32_t min;
    uint32_t max;
    uint32_t fuzz;
    uint32_t flat;
    uint32_t res;
} __attribute__((packed)) virtio_input_absinfo_t;

/* Initialize the virtio-input device. Returns true if found and initialized. */
bool virtio_input_init(void);

/* Check if virtio-input is available on PCI bus */
bool virtio_input_available(void);

/* Poll for and process pending input events.
 * Updates internal mouse state (position + buttons).
 * Call this regularly from the GUI loop. */
void virtio_input_poll(void);

/* Get current absolute mouse position (scaled to screen bounds) */
int  virtio_input_get_x(void);
int  virtio_input_get_y(void);

/* Get button state */
bool virtio_input_left_held(void);
bool virtio_input_right_held(void);
bool virtio_input_left_click(void);   /* Edge-triggered */
bool virtio_input_right_click(void);  /* Edge-triggered */

/* Set screen bounds for coordinate scaling */
void virtio_input_set_bounds(int max_x, int max_y);

/* Check if the driver is active */
bool virtio_input_is_active(void);

#endif
