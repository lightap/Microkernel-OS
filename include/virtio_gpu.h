#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include "types.h"
#include "virtio.h"

/* ===== VirtIO-GPU Feature Bits ===== */
#define VIRTIO_GPU_F_VIRGL      0   /* virgl 3D mode supported */
#define VIRTIO_GPU_F_EDID       1   /* EDID supported */

/* ===== VirtIO-GPU Command Types ===== */
/* 2D commands */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D        0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF            0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT               0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH            0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D       0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING   0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING   0x0107
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO           0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET                0x0109
#define VIRTIO_GPU_CMD_GET_EDID                  0x010A

/* Success responses */
#define VIRTIO_GPU_RESP_OK_NODATA               0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO         0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO          0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET               0x1103
#define VIRTIO_GPU_RESP_OK_EDID                 0x1104

/* Error responses */
#define VIRTIO_GPU_RESP_ERR_UNSPEC              0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY       0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID  0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID  0x1204
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER   0x1205

/* ===== Pixel Formats ===== */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM  1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM  2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM  3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM  4
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM  67
#define VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM  68
#define VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM  121
#define VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM  134

/* ===== GPU Queue Indices ===== */
#define VIRTIO_GPU_QUEUE_CONTROL  0
#define VIRTIO_GPU_QUEUE_CURSOR   1

/* ===== Max Scanouts ===== */
#define VIRTIO_GPU_MAX_SCANOUTS   16

/* 3D commands (virgl) */
#define VIRTIO_GPU_CMD_CTX_CREATE                0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY               0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE       0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE       0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D        0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D       0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D     0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D                 0x0207

#ifndef VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_2D
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_2D 0x0109
#endif

/* ===== Control Header (every command starts with this) ===== */
typedef struct {
    uint32_t type;          /* VIRTIO_GPU_CMD_* or VIRTIO_GPU_RESP_* */
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_ctrl_hdr_t;

/* ===== Rectangle ===== */
typedef struct {
    uint32_t x, y, width, height;
} __attribute__((packed)) virtio_gpu_rect_t;

/* ===== Display Info Response ===== */
typedef struct {
    virtio_gpu_rect_t r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed)) virtio_gpu_display_one_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_display_one_t pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed)) virtio_gpu_resp_display_info_t;

/* ===== Resource Create 2D ===== */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;        /* VIRTIO_GPU_FORMAT_* */
    uint32_t width;
    uint32_t height;
} __attribute__((packed)) virtio_gpu_resource_create_2d_t;

/* ===== Resource Attach Backing ===== */
typedef struct {
    uint64_t addr;          /* Physical address of page */
    uint32_t length;        /* Length of this entry */
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_mem_entry_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    /* Followed by nr_entries virtio_gpu_mem_entry_t entries */
} __attribute__((packed)) virtio_gpu_resource_attach_backing_t;

/* ===== Set Scanout ===== */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed)) virtio_gpu_set_scanout_t;

/* ===== Transfer to Host 2D ===== */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint64_t offset;        /* Byte offset into resource backing */
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_transfer_to_host_2d_t;

/* ===== Resource Flush ===== */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_flush_t;

/* ===== Resource Unref ===== */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_unref_t;

/* ===== Resource Detach Backing ===== */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_detach_backing_t;

typedef struct {
    uint32_t x, y, z;
    uint32_t w, h, d;
} __attribute__((packed)) virtio_gpu_box_t;

/* Transfer to/from host 3D */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_box_t box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
} __attribute__((packed)) virtio_gpu_transfer_host_3d_t;


/* ===== Public API ===== */

/* Initialize the virtio-gpu device. Returns true on success.
 * Also negotiates VIRGL feature if the device supports it. */
bool virtio_gpu_init(void);

/* Check if virtio-gpu was detected and initialized */
bool virtio_gpu_available(void);

/* Check if VIRGL 3D acceleration is available */
bool virtio_gpu_has_virgl(void);

/* Get pointer to the shared virtio device (for virgl driver) */
virtio_dev_t* virtio_gpu_get_device(void);

/* Set video mode. Allocates a GPU resource and framebuffer.
 * Returns true on success. After this call, virtio_gpu_get_fb()
 * returns a pointer to the framebuffer you can draw into. */
bool virtio_gpu_set_mode(uint16_t width, uint16_t height);

/* Get pointer to the framebuffer (identity-mapped, direct write) */
uint32_t* virtio_gpu_get_fb(void);

/* Get current resolution */
uint16_t virtio_gpu_get_width(void);
uint16_t virtio_gpu_get_height(void);

/* Flush a rectangle from guest memory to the display.
 * Call this after writing pixels to the framebuffer. */
void virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Flush the entire screen */
void virtio_gpu_flush_all(void);

/* Disable / clean up the GPU */
void virtio_gpu_disable(void);

#endif
