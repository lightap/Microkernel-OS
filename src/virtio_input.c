#include "virtio_input.h"
#include "virtio.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "heap.h"

/*
 * VirtIO Input Driver
 *
 * Handles virtio-tablet-pci and virtio-mouse-pci devices to provide
 * mouse input when PS/2 mouse doesn't work (e.g. with GL display).
 *
 * The device has two queues:
 *   Queue 0 (eventq): device writes input events to pre-posted buffers
 *   Queue 1 (statusq): driver writes status (LEDs etc.) — not used
 *
 * QEMU may expose MULTIPLE virtio-input PCI devices (keyboard, tablet)
 * all with the same PCI device ID 0x1052. We scan all of them and pick
 * the one that supports absolute positioning (the tablet).
 */

/* PCI device ID for modern virtio-input: 0x1040 + 18 = 0x1052 */
#define VIRTIO_PCI_DEV_INPUT    0x1052

/* Event queue index */
#define EVENTQ  0

/* Number of pre-posted event buffers */
#define NUM_EVENT_BUFS  64

/* Driver state */
static virtio_dev_t input_dev;
static bool input_initialized = false;

/* Pre-allocated event buffers (device writes into these) */
static virtio_input_event_t event_bufs[NUM_EVENT_BUFS] __attribute__((aligned(4096)));

/* Absolute axis ranges (defaults for QEMU virtio-tablet) */
static uint32_t abs_x_max = 32767;
static uint32_t abs_y_max = 32767;

/* Current state */
static volatile int vi_x = 0, vi_y = 0;
static volatile int vi_max_x = 1919, vi_max_y = 1079;
static volatile uint8_t vi_buttons = 0;
static volatile uint8_t vi_prev_buttons = 0;

/* Pending absolute values (before SYN_REPORT) */
static uint32_t pending_abs_x = 0, pending_abs_y = 0;
static bool has_pending_x = false, has_pending_y = false;

/* Debug counters */
static uint32_t total_events = 0;
static uint32_t debug_timer = 0;

/* ===== Post a single receive buffer to the eventq ===== */
static void post_event_buf(int buf_idx) {
    virtq_t* vq = &input_dev.queues[EVENTQ];

    if (vq->num_free < 1) return;

    /* Allocate a descriptor */
    uint16_t di = vq->free_head;
    vq->free_head = vq->desc[di].next;
    vq->num_free--;

    /* Set up as device-writable buffer */
    vq->desc[di].addr  = (uint64_t)(uint32_t)&event_bufs[buf_idx];
    vq->desc[di].len   = sizeof(virtio_input_event_t);
    vq->desc[di].flags = VRING_DESC_F_WRITE;
    vq->desc[di].next  = 0;

    /* Add to available ring */
    uint16_t avail_idx = vq->avail->idx % vq->size;
    vq->avail->ring[avail_idx] = di;

    __asm__ volatile ("mfence" ::: "memory");
    vq->avail->idx++;
}

/* ===== Pre-fill the eventq with receive buffers ===== */
static void prefill_eventq(void) {
    memset(event_bufs, 0, sizeof(event_bufs));

    int count = NUM_EVENT_BUFS;
    virtq_t* vq = &input_dev.queues[EVENTQ];
    if (count > (int)vq->size - 2)
        count = (int)vq->size - 2;

    for (int i = 0; i < count; i++) {
        post_event_buf(i);
    }

    /* Notify device that buffers are available */
    virtio_notify(&input_dev, EVENTQ);

    serial_printf("virtio-input: posted %d event buffers (queue size=%u free=%u)\n",
                  count, vq->size, vq->num_free);
}

/* ===== Read device config ===== */
static uint8_t input_cfg_read8(uint32_t offset) {
    if (!input_dev.device_cfg) return 0;
    return *(volatile uint8_t*)(input_dev.device_cfg + offset);
}

static void input_cfg_write8(uint32_t offset, uint8_t val) {
    if (!input_dev.device_cfg) return;
    *(volatile uint8_t*)(input_dev.device_cfg + offset) = val;
}

#define VCFG_SELECT  0
#define VCFG_SUBSEL  1
#define VCFG_SIZE    2
#define VCFG_DATA    8

static uint8_t query_config(uint8_t select, uint8_t subsel, void* buf, uint8_t buflen) {
    if (!input_dev.device_cfg) return 0;

    /* Write select first, then subsel (spec says write both, device updates on select) */
    input_cfg_write8(VCFG_SELECT, select);
    input_cfg_write8(VCFG_SUBSEL, subsel);

    __asm__ volatile ("mfence" ::: "memory");
    for (volatile int i = 0; i < 5000; i++) {}

    uint8_t size = input_cfg_read8(VCFG_SIZE);
    if (size == 0) return 0;

    uint8_t to_read = (size < buflen) ? size : buflen;
    uint8_t* dst = (uint8_t*)buf;
    for (uint8_t i = 0; i < to_read; i++) {
        dst[i] = input_cfg_read8(VCFG_DATA + i);
    }
    return size;
}

/* ===== Query abs axis info ===== */
static void query_abs_info(void) {
    virtio_input_absinfo_t info;

    memset(&info, 0, sizeof(info));
    if (query_config(VIRTIO_INPUT_CFG_ABS_INFO, ABS_X, &info, sizeof(info)) > 0) {
        if (info.max > 0) abs_x_max = info.max;
        serial_printf("virtio-input: ABS_X max=%u\n", abs_x_max);
    }

    memset(&info, 0, sizeof(info));
    if (query_config(VIRTIO_INPUT_CFG_ABS_INFO, ABS_Y, &info, sizeof(info)) > 0) {
        if (info.max > 0) abs_y_max = info.max;
        serial_printf("virtio-input: ABS_Y max=%u\n", abs_y_max);
    }
}

/* ===== Check if this device is a pointing device (not a keyboard) ===== */
static bool check_device_is_pointer(void) {
    if (!input_dev.device_cfg) {
        serial_printf("virtio-input: no device_cfg, assuming tablet\n");
        return true;  /* Assume usable if we can't query */
    }

    /* Try to get the device name */
    char name[64];
    memset(name, 0, sizeof(name));
    if (query_config(VIRTIO_INPUT_CFG_ID_NAME, 0, name, sizeof(name) - 1) > 0) {
        serial_printf("virtio-input: device name: '%s'\n", name);
        /* Reject if "keyboard" or "Keyboard" appears in name */
        for (int i = 0; name[i]; i++) {
            if ((name[i] == 'k' || name[i] == 'K') &&
                (strncmp(&name[i], "keyboard", 8) == 0 ||
                 strncmp(&name[i], "Keyboard", 8) == 0)) {
                serial_printf("virtio-input: this is a keyboard, skipping\n");
                return false;
            }
        }
    }

    /* Check event types bitmap */
    uint8_t ev_bits[4];
    memset(ev_bits, 0, sizeof(ev_bits));
    if (query_config(VIRTIO_INPUT_CFG_EV_BITS, 0, ev_bits, sizeof(ev_bits)) > 0) {
        serial_printf("virtio-input: ev_bits[0]=%02x\n", ev_bits[0]);
        bool has_abs = (ev_bits[0] & (1 << EV_ABS)) != 0;
        bool has_rel = (ev_bits[0] & (1 << EV_REL)) != 0;
        bool has_key = (ev_bits[0] & (1 << EV_KEY)) != 0;
        serial_printf("virtio-input: abs=%d rel=%d key=%d\n", has_abs, has_rel, has_key);
        if (has_abs || has_rel) return true;
        /* Only KEY = keyboard */
        if (has_key && !has_abs && !has_rel) return false;
    }

    /* If we can't determine, accept it */
    return true;
}

/* ===== Process a single input event ===== */
static void process_event(virtio_input_event_t* ev) {
    switch (ev->type) {
        case EV_ABS:
            if (ev->code == ABS_X) {
                pending_abs_x = ev->value;
                has_pending_x = true;
            } else if (ev->code == ABS_Y) {
                pending_abs_y = ev->value;
                has_pending_y = true;
            }
            break;

        case EV_REL:
            if (ev->code == REL_X) {
                vi_x += (int32_t)ev->value;
                if (vi_x < 0) vi_x = 0;
                if (vi_x > vi_max_x) vi_x = vi_max_x;
            } else if (ev->code == REL_Y) {
                vi_y += (int32_t)ev->value;
                if (vi_y < 0) vi_y = 0;
                if (vi_y > vi_max_y) vi_y = vi_max_y;
            }
            break;

        case EV_KEY:
            if (ev->code == BTN_LEFT) {
                if (ev->value) vi_buttons |= 0x01;
                else           vi_buttons &= ~0x01;
            } else if (ev->code == BTN_RIGHT) {
                if (ev->value) vi_buttons |= 0x02;
                else           vi_buttons &= ~0x02;
            } else if (ev->code == BTN_MIDDLE) {
                if (ev->value) vi_buttons |= 0x04;
                else           vi_buttons &= ~0x04;
            }
            break;

        case EV_SYN:
            if (ev->code == SYN_REPORT) {
                /*
                 * Apply any pending absolute coordinates.
                 * This is NOT gated on any flag — if we received
                 * EV_ABS events, always apply them here.
                 */
                if (has_pending_x && abs_x_max > 0) {
                    uint32_t ax = pending_abs_x;
                    if (ax > abs_x_max) ax = abs_x_max;
                    vi_x = (int)((ax * (uint32_t)vi_max_x) / abs_x_max);
                }
                if (has_pending_y && abs_y_max > 0) {
                    uint32_t ay = pending_abs_y;
                    if (ay > abs_y_max) ay = abs_y_max;
                    vi_y = (int)((ay * (uint32_t)vi_max_y) / abs_y_max);
                }
                has_pending_x = false;
                has_pending_y = false;
            }
            break;
    }
}

/* ===== Public API ===== */

bool virtio_input_available(void) {
    if (input_initialized) return true;
    pci_device_t* dev = pci_find_device(VIRTIO_PCI_VENDOR, VIRTIO_PCI_DEV_INPUT);
    return dev != NULL;
}

bool virtio_input_init(void) {
    if (input_initialized) return true;

    serial_printf("virtio-input: === INIT START ===\n");

    /*
     * QEMU can expose multiple virtio-input PCI devices with the same
     * device ID 0x1052 (e.g. keyboard + tablet). We try each one until
     * we find a pointing device (tablet or mouse), skipping keyboards.
     */
    pci_device_t* chosen_pci = NULL;

    for (int n = 0; n < 8; n++) {
        pci_device_t* pci = pci_find_device_nth(VIRTIO_PCI_VENDOR,
                                                  VIRTIO_PCI_DEV_INPUT, n);
        if (!pci) {
            serial_printf("virtio-input: no more 0x1052 devices (checked %d)\n", n);
            break;
        }

        serial_printf("virtio-input: trying device #%d at %d:%d.%d (irq=%d)\n",
                      n, pci->bus, pci->slot, pci->func, pci->irq_line);

        /* Try to initialize this device */
        memset(&input_dev, 0, sizeof(input_dev));
        if (!virtio_init_pci(&input_dev, pci)) {
            serial_printf("virtio-input: transport init failed for #%d\n", n);
            continue;
        }

        /* Set up eventq */
        if (!virtio_setup_queue(&input_dev, EVENTQ)) {
            serial_printf("virtio-input: eventq setup failed for #%d\n", n);
            continue;
        }

        /* Set DRIVER_OK so we can query device config */
        virtio_driver_ok(&input_dev);

        /* Check if this is a pointing device (not a keyboard) */
        if (check_device_is_pointer()) {
            serial_printf("virtio-input: device #%d is a pointer — using it\n", n);
            chosen_pci = pci;
            break;
        }

        /* Reset this device and try the next one */
        serial_printf("virtio-input: device #%d rejected, trying next\n", n);
        if (input_dev.common_cfg) {
            *(volatile uint8_t*)(input_dev.common_cfg + 0x14) = 0;  /* RESET */
        }
    }

    if (!chosen_pci) {
        serial_printf("virtio-input: no pointing device found!\n");
        return false;
    }

    /* Query axis ranges (use defaults if query fails) */
    query_abs_info();

    /* Pre-fill event queue with receive buffers */
    prefill_eventq();

    /* Set initial position to center */
    vi_x = vi_max_x / 2;
    vi_y = vi_max_y / 2;

    input_initialized = true;
    serial_printf("virtio-input: === INIT OK === abs_max=%u,%u screen=%d,%d\n",
                  abs_x_max, abs_y_max, vi_max_x, vi_max_y);
    return true;
}

void virtio_input_poll(void) {
    if (!input_initialized) return;

    virtq_t* vq = &input_dev.queues[EVENTQ];

    /* Memory barrier to see device's writes */
    __asm__ volatile ("mfence" ::: "memory");

    /* Save previous button state */
    vi_prev_buttons = vi_buttons;

    /* Process all completed event buffers */
    int processed = 0;
    while (vq->last_used_idx != vq->used->idx) {
        uint16_t used_slot = vq->last_used_idx % vq->size;
        uint32_t desc_idx = vq->used->ring[used_slot].id;

        virtio_input_event_t* ev = (virtio_input_event_t*)(uint32_t)vq->desc[desc_idx].addr;

        process_event(ev);
        total_events++;
        processed++;

        /* Free descriptor */
        vq->desc[desc_idx].next = vq->free_head;
        vq->free_head = desc_idx;
        vq->num_free++;
        vq->last_used_idx++;

        /* Re-post this buffer */
        int buf_idx = ((uint32_t)ev - (uint32_t)event_bufs) / sizeof(virtio_input_event_t);
        if (buf_idx >= 0 && buf_idx < NUM_EVENT_BUFS) {
            memset(ev, 0, sizeof(*ev));
            post_event_buf(buf_idx);
        }
    }

    if (processed > 0) {
        virtio_notify(&input_dev, EVENTQ);
    }

    /* Periodic debug — logs every ~500 poll cycles */
    debug_timer++;
    if (debug_timer >= 500) {
        debug_timer = 0;
        serial_printf("vi: xy=%d,%d btn=%x tot=%u used=%u avail=%u\n",
                      vi_x, vi_y, vi_buttons, total_events,
                      vq->used->idx, vq->avail->idx);
    }
}

int  virtio_input_get_x(void) { return vi_x; }
int  virtio_input_get_y(void) { return vi_y; }

bool virtio_input_left_held(void)  { return (vi_buttons & 0x01) != 0; }
bool virtio_input_right_held(void) { return (vi_buttons & 0x02) != 0; }

bool virtio_input_left_click(void) {
    return (vi_buttons & 0x01) && !(vi_prev_buttons & 0x01);
}
bool virtio_input_right_click(void) {
    return (vi_buttons & 0x02) && !(vi_prev_buttons & 0x02);
}

void virtio_input_set_bounds(int max_x, int max_y) {
    vi_max_x = max_x;
    vi_max_y = max_y;
    if (vi_x > vi_max_x) vi_x = vi_max_x;
    if (vi_y > vi_max_y) vi_y = vi_max_y;
    serial_printf("virtio-input: bounds set to %d x %d\n", max_x, max_y);
}

bool virtio_input_is_active(void) {
    return input_initialized;
}
