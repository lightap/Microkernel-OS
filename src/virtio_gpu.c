#include "virtio_gpu.h"
#include "virtio.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "vga.h"
#include "serial.h"
#include "heap.h"
#include <stdint.h>

extern uint16_t virtq_alloc_desc_chain(virtq_t* vq, uint16_t count);
extern void virtq_add_to_avail(virtq_t* vq, uint16_t head);
extern void virtio_notify(virtio_dev_t* dev, uint16_t queue_idx);
extern bool virtio_poll(virtio_dev_t* dev, uint16_t queue_idx);

/*
 * VirtIO-GPU 2D Driver
 *
 * This implements the virtio-gpu device for 2D framebuffer rendering.
 * The driver creates a GPU resource, attaches guest memory as backing
 * store, and uses transfer + flush commands to display pixels.
 *
 * QEMU usage:
 *   qemu-system-i386 -kernel microkernel.bin -m 2G -device virtio-gpu-pci
 *
 * The framebuffer is allocated in guest RAM (identity-mapped) and the
 * driver tells the GPU to read from it on each flush.
 */

/* ===== Driver State ===== */
static virtio_dev_t gpu_dev;
static bool gpu_initialized = false;
static bool gpu_has_virgl = false;

static uint32_t* gpu_framebuffer = NULL;    /* Guest-side framebuffer */
static uint32_t  gpu_fb_phys = 0;           /* Physical addr of FB */
static uint32_t  gpu_fb_size = 0;           /* Size in bytes */
static uint16_t  gpu_width = 0;
static uint16_t  gpu_height = 0;
static uint32_t  gpu_resource_id = 1;       /* Resource ID counter */
static uint32_t  gpu_active_resource = 0;   /* Currently active resource */
#define EINVAL      22   /* Invalid argument */
#define EAGAIN      11   /* Resource temporarily unavailable */
#define ETIMEDOUT   110  /* Connection timed out */
#define ENOMEM      12   /* Out of memory */

/*
 * Command buffer: we use a statically allocated region for building
 * GPU commands. Each command is sent as an output descriptor, and
 * the response comes back through an input descriptor.
 *
 * We use page-aligned memory so addresses are clean for the device.
 */
#define CMD_BUF_SIZE  4096
static uint8_t cmd_buf[CMD_BUF_SIZE] __attribute__((aligned(4096)));
static uint8_t resp_buf[CMD_BUF_SIZE] __attribute__((aligned(4096)));

/* ===== Send a GPU Command and Wait for Response ===== */
static bool gpu_cmd(void* cmd, uint32_t cmd_len, void* resp, uint32_t resp_len) {
    /* Copy command to the aligned command buffer */
    memcpy(cmd_buf, cmd, cmd_len);

    /* Clear response buffer */
    memset(resp_buf, 0, resp_len);

    /* Submit to control queue */
    int head = virtio_send(&gpu_dev, VIRTIO_GPU_QUEUE_CONTROL,
                           (uint32_t)cmd_buf, cmd_len,
                           (uint32_t)resp_buf, resp_len);
    if (head < 0) {
        serial_printf("virtio-gpu: failed to submit command\n");
        return false;
    }

    /* Notify device */
    virtio_notify(&gpu_dev, VIRTIO_GPU_QUEUE_CONTROL);

    /* Wait for response */
    virtio_wait(&gpu_dev, VIRTIO_GPU_QUEUE_CONTROL);

    /* Copy response back */
    memcpy(resp, resp_buf, resp_len);

    /* Check response type */
    virtio_gpu_ctrl_hdr_t* hdr = (virtio_gpu_ctrl_hdr_t*)resp;
    if (hdr->type >= VIRTIO_GPU_RESP_ERR_UNSPEC) {
        serial_printf("virtio-gpu: command error, type=%x\n", hdr->type);
        return false;
    }

    return true;
}

/* Simpler version for commands that only return a basic OK header */
static bool gpu_cmd_ok(void* cmd, uint32_t cmd_len) {
    virtio_gpu_ctrl_hdr_t resp;
    return gpu_cmd(cmd, cmd_len, &resp, sizeof(resp));
}

/* ===== Get Display Info ===== */
static bool gpu_get_display_info(uint32_t* width, uint32_t* height) {
    virtio_gpu_ctrl_hdr_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    virtio_gpu_resp_display_info_t resp;
    memset(&resp, 0, sizeof(resp));

    if (!gpu_cmd(&cmd, sizeof(cmd), &resp, sizeof(resp))) {
        serial_printf("virtio-gpu: GET_DISPLAY_INFO failed\n");
        return false;
    }

    /* Find the first enabled display */
    for (int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        if (resp.pmodes[i].enabled) {
            *width = resp.pmodes[i].r.width;
            *height = resp.pmodes[i].r.height;
            serial_printf("virtio-gpu: display %d: %ux%u (enabled)\n",
                          i, *width, *height);
            return true;
        }
    }

    /* No display enabled - use a default */
    serial_printf("virtio-gpu: no enabled display, using defaults\n");
    *width = 1024;
    *height = 768;
    return true;
}

/* ===== Create a 2D Resource ===== */
static bool gpu_create_resource(uint32_t res_id, uint32_t format,
                                 uint32_t width, uint32_t height) {
    virtio_gpu_resource_create_2d_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd.resource_id = res_id;
    cmd.format = format;
    cmd.width = width;
    cmd.height = height;

    serial_printf("virtio-gpu: creating resource %u (%ux%u, fmt=%u)\n",
                  res_id, width, height, format);

    return gpu_cmd_ok(&cmd, sizeof(cmd));
}

/* ===== Attach Backing Memory to Resource ===== */
static bool gpu_attach_backing(uint32_t res_id, uint32_t phys_addr, uint32_t size) {
    /*
     * We need to send the attach_backing header followed by the
     * memory entries array. Pack them into a single buffer.
     */
    struct {
        virtio_gpu_resource_attach_backing_t hdr;
        virtio_gpu_mem_entry_t entry;
    } __attribute__((packed)) cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd.hdr.resource_id = res_id;
    cmd.hdr.nr_entries = 1;
    cmd.entry.addr = (uint64_t)phys_addr;
    cmd.entry.length = size;

    serial_printf("virtio-gpu: attaching backing %x (%u bytes) to resource %u\n",
                  phys_addr, size, res_id);

    return gpu_cmd_ok(&cmd, sizeof(cmd));
}

/* ===== Set Scanout (connect resource to display) ===== */
static bool gpu_set_scanout(uint32_t scanout_id, uint32_t res_id,
                             uint32_t w, uint32_t h) {
    virtio_gpu_set_scanout_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd.r.x = 0;
    cmd.r.y = 0;
    cmd.r.width = w;
    cmd.r.height = h;
    cmd.scanout_id = scanout_id;
    cmd.resource_id = res_id;

    serial_printf("virtio-gpu: set scanout %u -> resource %u (%ux%u)\n",
                  scanout_id, res_id, w, h);

    return gpu_cmd_ok(&cmd, sizeof(cmd));
}

/* ===== Transfer from Guest to Host ===== */
static bool gpu_transfer(uint32_t res_id, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h) {
    virtio_gpu_transfer_to_host_2d_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd.r.x = x;
    cmd.r.y = y;
    cmd.r.width = w;
    cmd.r.height = h;
    cmd.offset = ((uint64_t)y * gpu_width + x) * 4;
    cmd.resource_id = res_id;

    return gpu_cmd_ok(&cmd, sizeof(cmd));
}

/* ===== Flush Resource to Display ===== */
static bool gpu_flush_resource(uint32_t res_id, uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h) {
    virtio_gpu_resource_flush_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd.r.x = x;
    cmd.r.y = y;
    cmd.r.width = w;
    cmd.r.height = h;
    cmd.resource_id = res_id;

    return gpu_cmd_ok(&cmd, sizeof(cmd));
}

/* ===== Destroy a Resource ===== */
static bool gpu_destroy_resource(uint32_t res_id) {
    virtio_gpu_resource_detach_backing_t detach;
    memset(&detach, 0, sizeof(detach));
    detach.hdr.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    detach.resource_id = res_id;
    gpu_cmd_ok(&detach, sizeof(detach));

    virtio_gpu_resource_unref_t unref;
    memset(&unref, 0, sizeof(unref));
    unref.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    unref.resource_id = res_id;
    return gpu_cmd_ok(&unref, sizeof(unref));
}

/* ===== Allocate Framebuffer Memory ===== */
static uint32_t* alloc_framebuffer(uint32_t size) {
    uint32_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    serial_printf("virtio-gpu: allocating %u pages for framebuffer\n", num_pages);

    /* Try to allocate contiguous pages from PMM */
    uint32_t first_page = 0;

    /*
     * Strategy: allocate pages one at a time and hope they're contiguous.
     * PMM bitmap allocator tends to give sequential pages. We verify
     * contiguity and fall back to kmalloc if needed.
     */
    void* pages[1024];  /* Max ~4MB framebuffer */
    if (num_pages > 1024) {
        serial_printf("virtio-gpu: framebuffer too large\n");
        return NULL;
    }

    for (uint32_t i = 0; i < num_pages; i++) {
        pages[i] = pmm_alloc_page();
        if (!pages[i]) {
            /* Free what we got */
            for (uint32_t j = 0; j < i; j++) pmm_free_page(pages[j]);
            serial_printf("virtio-gpu: out of memory for FB\n");
            return NULL;
        }
        if (i == 0) {
            first_page = (uint32_t)pages[0];
        } else if ((uint32_t)pages[i] != first_page + i * PAGE_SIZE) {
            /* Not contiguous - this is a problem for DMA.
             * Free everything and use kmalloc instead. */
            serial_printf("virtio-gpu: non-contiguous pages, using kmalloc\n");
            for (uint32_t j = 0; j <= i; j++) pmm_free_page(pages[j]);

            /* kmalloc with page alignment */
            uint8_t* buf = (uint8_t*)kmalloc(size + PAGE_SIZE);
            if (!buf) return NULL;
            buf = (uint8_t*)(((uint32_t)buf + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
            return (uint32_t*)buf;
        }
    }

    /* Zero the framebuffer */
    memset((void*)first_page, 0, size);

    serial_printf("virtio-gpu: framebuffer at phys %x (%u KB)\n",
                  first_page, size / 1024);
    return (uint32_t*)first_page;
}

/* ===== Public API ===== */

bool virtio_gpu_init(void) {
    if (gpu_initialized) return true;

    serial_printf("virtio-gpu: initializing...\n");

    /*
     * Initialize with VIRGL feature requested (bit 0).
     * If the device supports 3D (virtio-gpu-gl-pci), we get virgl.
     * If not (plain virtio-gpu-pci), the bit is silently dropped.
     */
    if (!virtio_init_features(&gpu_dev, VIRTIO_PCI_DEV_GPU,
                              (1U << VIRTIO_GPU_F_VIRGL))) {
        serial_printf("virtio-gpu: transport init failed\n");
        return false;
    }

    /* Check if VIRGL was accepted */
    volatile uint8_t* cfg = gpu_dev.common_cfg;
    *(volatile uint32_t*)(cfg + VIRTIO_COMMON_GFSELECT) = 0;
    uint32_t accepted = *(volatile uint32_t*)(cfg + VIRTIO_COMMON_GF);
    /* Re-read the DEVICE features to check what was offered */
    *(volatile uint32_t*)(cfg + VIRTIO_COMMON_DFSELECT) = 0;
    uint32_t offered = *(volatile uint32_t*)(cfg + VIRTIO_COMMON_DF);
    gpu_has_virgl = !!(offered & (1U << VIRTIO_GPU_F_VIRGL));
    serial_printf("virtio-gpu: VIRGL %s\n",
                  gpu_has_virgl ? "available" : "not available");

    /* Set up the control queue (queue 0) */
    if (!virtio_setup_queue(&gpu_dev, VIRTIO_GPU_QUEUE_CONTROL)) {
        serial_printf("virtio-gpu: control queue setup failed\n");
        return false;
    }

    /* Optionally set up cursor queue (queue 1) - not required */
    if (gpu_dev.num_queues > 1) {
        virtio_setup_queue(&gpu_dev, VIRTIO_GPU_QUEUE_CURSOR);
    }

    /* Tell device we're ready */
    virtio_driver_ok(&gpu_dev);

    gpu_initialized = true;
    serial_printf("virtio-gpu: initialized successfully!\n");
    return true;
}

/* ===== Getters for shared device access (used by virgl.c) ===== */

virtio_dev_t* virtio_gpu_get_device(void) {
    return gpu_initialized ? &gpu_dev : NULL;
}

bool virtio_gpu_has_virgl(void) {
    return gpu_has_virgl;
}

bool virtio_gpu_available(void) {
    if (gpu_initialized) return true;

    /* Quick check: is there a virtio-gpu on the PCI bus? */
    pci_device_t* dev = pci_find_device(VIRTIO_PCI_VENDOR, VIRTIO_PCI_DEV_GPU);
    if (dev) return true;

    /* Check transitional devices */
    for (uint16_t id = VIRTIO_PCI_DEV_TRANS_LO; id <= VIRTIO_PCI_DEV_TRANS_HI; id++) {
        dev = pci_find_device(VIRTIO_PCI_VENDOR, id);
        if (dev) {
            uint32_t subsys = pci_read(dev->bus, dev->slot, dev->func, 0x2C);
            uint16_t subsys_dev = (subsys >> 16) & 0xFFFF;
            if (subsys_dev == 16) return true;
        }
    }

    return false;
}

bool virtio_gpu_send_cmd_2iov(const void *hdr, uint32_t hdr_len,
                              const void *data, uint32_t data_len,
                              void *resp, uint32_t resp_len)
{
    if (!hdr || hdr_len == 0) return false;

    uint32_t total = hdr_len + data_len;

    /* We must submit header+payload as ONE virtio-gpu control request.
       Your transport only supports one OUT buffer, so we pack. */

    uint8_t *out = NULL;

    if (total <= CMD_BUF_SIZE) {
        out = cmd_buf; /* use the aligned static buffer */
    } else {
        /* Fallback: allocate a temporary contiguous buffer.
           Assumes kmalloc memory is DMA-visible/identity-mapped in your kernel. */
        out = (uint8_t*)kmalloc(total);
        if (!out) {
            serial_printf("virtio-gpu: send_cmd_2iov ENOMEM total=%u\n", total);
            return false;
        }
    }

    /* Build contiguous request */
    memcpy(out, hdr, hdr_len);
    if (data_len) memcpy(out + hdr_len, data, data_len);

    /* Prepare response */
    if (resp_len > CMD_BUF_SIZE) {
        serial_printf("virtio-gpu: resp too big (%u)\n", resp_len);
        if (out != cmd_buf) kfree(out);
        return false;
    }
    memset(resp_buf, 0, resp_len);

    /* Submit to control queue */
    int head = virtio_send(&gpu_dev, VIRTIO_GPU_QUEUE_CONTROL,
                           (uint32_t)out, total,
                           (uint32_t)resp_buf, resp_len);
    if (head < 0) {
        serial_printf("virtio-gpu: failed to submit 2iov cmd\n");
        if (out != cmd_buf) kfree(out);
        return false;
    }

    virtio_notify(&gpu_dev, VIRTIO_GPU_QUEUE_CONTROL);
    virtio_wait(&gpu_dev, VIRTIO_GPU_QUEUE_CONTROL);

    /* Copy response back */
    if (resp && resp_len) {
        memcpy(resp, resp_buf, resp_len);

        virtio_gpu_ctrl_hdr_t *rh = (virtio_gpu_ctrl_hdr_t*)resp;
        if (rh->type >= VIRTIO_GPU_RESP_ERR_UNSPEC) {
            serial_printf("virtio-gpu: command error, type=%x\n", rh->type);
            if (out != cmd_buf) kfree(out);
            return false;
        }
    }

    if (out != cmd_buf) kfree(out);
    return true;
}


bool virtio_gpu_set_mode(uint16_t width, uint16_t height) {
    if (!gpu_initialized && !virtio_gpu_init()) return false;

    /* If we have an existing resource, destroy it */
    if (gpu_active_resource) {
        gpu_set_scanout(0, 0, 0, 0);  /* Disable scanout */
        gpu_destroy_resource(gpu_active_resource);
        gpu_active_resource = 0;
    }

    /* If caller passed 0,0 — query the display's preferred resolution */
    if (width == 0 || height == 0) {
        uint32_t dw = 0, dh = 0;
        gpu_get_display_info(&dw, &dh);
        if (dw > 0 && dh > 0) {
            width = (uint16_t)dw;
            height = (uint16_t)dh;
        } else {
            width = 1024;
            height = 768;
        }
    }

    gpu_width = width;
    gpu_height = height;
    gpu_fb_size = (uint32_t)width * height * 4;  /* 32bpp BGRX */

    /* Allocate the guest-side framebuffer */
    gpu_framebuffer = alloc_framebuffer(gpu_fb_size);
    if (!gpu_framebuffer) {
        serial_printf("virtio-gpu: failed to allocate framebuffer\n");
        return false;
    }
    gpu_fb_phys = (uint32_t)gpu_framebuffer;  /* Identity mapped */

    /* Create a GPU resource */
    uint32_t res_id = gpu_resource_id++;
    if (!gpu_create_resource(res_id, VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM,
                              width, height)) {
        serial_printf("virtio-gpu: create resource failed\n");
        return false;
    }

    /* Attach our framebuffer memory as backing store */
    if (!gpu_attach_backing(res_id, gpu_fb_phys, gpu_fb_size)) {
        serial_printf("virtio-gpu: attach backing failed\n");
        gpu_destroy_resource(res_id);
        return false;
    }

    /* Connect the resource to scanout 0 (primary display) */
    if (!gpu_set_scanout(0, res_id, width, height)) {
        serial_printf("virtio-gpu: set scanout failed\n");
        gpu_destroy_resource(res_id);
        return false;
    }

    gpu_active_resource = res_id;

    /* Initial transfer + flush to show the (blank) framebuffer */
    gpu_transfer(res_id, 0, 0, width, height);
    gpu_flush_resource(res_id, 0, 0, width, height);

    kprintf("virtio-gpu: mode set to %ux%ux32\n", width, height);
    return true;
}

uint32_t* virtio_gpu_get_fb(void) {
    return gpu_framebuffer;
}

uint16_t virtio_gpu_get_width(void) {
    return gpu_width;
}

uint16_t virtio_gpu_get_height(void) {
    return gpu_height;
}

void virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!gpu_active_resource || !gpu_framebuffer) return;

    /* Clamp to screen bounds */
    if (x >= gpu_width || y >= gpu_height) return;
    if (x + w > gpu_width)  w = gpu_width - x;
    if (y + h > gpu_height) h = gpu_height - y;

    /* Transfer guest memory -> host resource, then flush to display */
    gpu_transfer(gpu_active_resource, x, y, w, h);
    gpu_flush_resource(gpu_active_resource, x, y, w, h);
}

void virtio_gpu_flush_all(void) {
    virtio_gpu_flush(0, 0, gpu_width, gpu_height);
}

void virtio_gpu_disable(void) {
    if (!gpu_initialized) return;

    if (gpu_active_resource) {
        gpu_set_scanout(0, 0, 0, 0);
        gpu_destroy_resource(gpu_active_resource);
        gpu_active_resource = 0;
    }

    /* Reset device */
    if (gpu_dev.common_cfg) {
        *(volatile uint8_t*)(gpu_dev.common_cfg + VIRTIO_COMMON_STATUS) = 0;
    }

    gpu_framebuffer = NULL;
    gpu_fb_phys = 0;
    gpu_fb_size = 0;
    gpu_width = 0;
    gpu_height = 0;
    gpu_initialized = false;
}
/*
int virgl_submit_command(const void* buf, uint32_t num_dwords)
{
    if (!buf || num_dwords == 0) return -22;  // -EINVAL

    // Use existing virtio_send on control queue (queue 0) 
    int ret = virtio_send(&gpu_dev, 0,               // queue 0 = control
                          (uint32_t)buf, num_dwords * 4,  // out only
                          0, 0);                          // no in buffer

    if (ret < 0) {
        serial_printf("virgl_submit: virtio_send failed (%d)\n", ret);
        return ret;
    }

    // Wait for GPU to process it 
    virtio_wait(&gpu_dev, 0);

    return 0;
}*/

// src/virgl.c

/* Helper to flush cache line */
static inline void clflush(const void *p) {
    __asm__ volatile("clflush (%0)" : : "r"(p));
}

// src/virgl.c

// src/virgl.c
// src/virgl.c
// src/virgl.c

int virgl_submit_command(const void* buf, uint32_t num_dwords) {
    if (!buf || num_dwords < 6) return -22;

    uint32_t* dwords = (uint32_t*)buf;
    uint32_t type = dwords[0];

    /* 
     * TARGET FIX: Only patch 3D commands.
     * 0x0200 - 0x02FF: VirGL 3D commands (Submit, Create, etc.)
     * 0x0100 - 0x01FF: Standard 2D commands (Flush, Transfer2D)
     */
    if (type >= 0x0200 && type <= 0x02ff) {
        dwords[4] = 1; // 3D Context
    } else {
        dwords[4] = 0; // Display/Global Context
    }

    // Ensure CPU memory writes are visible to the GPU
    __asm__ volatile ("mfence" ::: "memory");

    int ret = virtio_send(&gpu_dev, 0, (uint32_t)buf, num_dwords * 4, 0, 0);
    if (ret < 0) return ret;

    virtio_wait(&gpu_dev, 0);
    return 0;
}