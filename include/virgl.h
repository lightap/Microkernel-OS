#ifndef VIRGL_H
#define VIRGL_H

#include "types.h"
#include "virtio_gpu.h"

#include <stdint.h>
#include <stdbool.h>

/* ============================================================
 * VirtIO-GPU 3D commands
 * ============================================================ */
#define VIRTIO_GPU_CMD_CTX_CREATE              0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY             0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE     0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE     0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D      0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D     0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D   0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D               0x0207

/* ============================================================
 * VirtIO-GPU 3D structs (must match QEMU expectations)
 * ============================================================ */

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;   /* 24 bytes */
    uint32_t nlen;               /* length of debug_name including NUL */
    uint32_t context_init;       /* must be 0 for standard virgl */
    char     debug_name[64];
} __attribute__((packed)) virtio_gpu_ctx_create_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t target;       /* PIPE_TEXTURE_2D, PIPE_BUFFER, etc. */
    uint32_t format;       /* VIRGL_FORMAT_* ids (virgl_hw.h / gallium subset) */
    uint32_t bind;         /* VIRGL_BIND_* */
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t padding;      /* must exist: QEMU expects 72 bytes total */
} __attribute__((packed)) virtio_gpu_resource_create_3d_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_ctx_resource_t;

/* SUBMIT_3D must be 32 bytes (hdr 24 + size 4 + padding 4) */
typedef struct __attribute__((packed)) virtio_gpu_cmd_submit_3d {
    virtio_gpu_ctrl_hdr_t hdr;  /* 24 */
    uint32_t size;              /* SIZE IN BYTES (virtio-gpu) */
    uint32_t padding;           /* required */
} virtio_gpu_cmd_submit_3d_t;

_Static_assert(sizeof(virtio_gpu_ctrl_hdr_t) == 24, "ctrl_hdr must be 24 bytes");
_Static_assert(sizeof(virtio_gpu_ctx_create_t) == 96, "ctx_create must be 96 bytes");
_Static_assert(sizeof(virtio_gpu_resource_create_3d_t) == 72, "res_create_3d must be 72 bytes");
_Static_assert(sizeof(virtio_gpu_ctx_resource_t) == 32, "ctx_resource must be 32 bytes");
_Static_assert(sizeof(virtio_gpu_cmd_submit_3d_t) == 32, "SUBMIT_3D must be 32 bytes");

/* ============================================================
 * Virgl commands (CCMD)
 * ============================================================ */
#define VIRGL_CCMD_NOP                      0
#define VIRGL_CCMD_CREATE_OBJECT            1
#define VIRGL_CCMD_BIND_OBJECT              2
#define VIRGL_CCMD_DESTROY_OBJECT           3
#define VIRGL_CCMD_SET_VIEWPORT_STATE       4
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE    5
#define VIRGL_CCMD_SET_VERTEX_BUFFERS       6
#define VIRGL_CCMD_CLEAR                    7
#define VIRGL_CCMD_DRAW_VBO                 8
#define VIRGL_CCMD_RESOURCE_INLINE_WRITE    9
#define VIRGL_CCMD_SET_SAMPLER_VIEWS        10
#define VIRGL_CCMD_SET_INDEX_BUFFER         11
#define VIRGL_CCMD_SET_CONSTANT_BUFFER      12
#define VIRGL_CCMD_SET_STENCIL_REF          13
#define VIRGL_CCMD_SET_BLEND_COLOR          14
#define VIRGL_CCMD_SET_SCISSOR_STATE        15
#define VIRGL_CCMD_BLIT                     16
#define VIRGL_CCMD_RESOURCE_COPY_REGION     17
#define VIRGL_CCMD_BIND_SAMPLER_STATES      18
#define VIRGL_CCMD_BEGIN_QUERY              19
#define VIRGL_CCMD_END_QUERY                20
#define VIRGL_CCMD_GET_QUERY_RESULT         21
#define VIRGL_CCMD_SET_POLYGON_STIPPLE      22
#define VIRGL_CCMD_SET_CLIP_STATE           23
#define VIRGL_CCMD_SET_SAMPLE_MASK          24
#define VIRGL_CCMD_SET_STREAMOUT_TARGETS    25
#define VIRGL_CCMD_SET_RENDER_CONDITION     26
#define VIRGL_CCMD_SET_UNIFORM_BUFFER       27
#define VIRGL_CCMD_SET_SUB_CTX              28
#define VIRGL_CCMD_CREATE_SUB_CTX           29
#define VIRGL_CCMD_DESTROY_SUB_CTX          30
#define VIRGL_CCMD_BIND_SHADER              31

/* ============================================================
 * Virgl object types (virgl_protocol.h enum virgl_object_type)
 * ============================================================ */
#define VIRGL_OBJECT_NULL                   0
#define VIRGL_OBJECT_BLEND                  1
#define VIRGL_OBJECT_RASTERIZER             2
#define VIRGL_OBJECT_DSA                    3
#define VIRGL_OBJECT_SHADER                 4
#define VIRGL_OBJECT_VERTEX_ELEMENTS        5
#define VIRGL_OBJECT_SAMPLER_VIEW           6
#define VIRGL_OBJECT_SAMPLER_STATE          7
#define VIRGL_OBJECT_SURFACE                8
#define VIRGL_OBJECT_QUERY                  9
#define VIRGL_OBJECT_STREAMOUT_TARGET       10

/* ============================================================
 * Virgl wire header packing (VIRGL_CMD0):
 * bits  0..7  = cmd
 * bits  8..15 = obj
 * bits 16..31 = len (dwords, payload only, not including header)
 * ============================================================ */
#ifdef VIRGL_CMD_HDR
#undef VIRGL_CMD_HDR
#endif
#define VIRGL_CMD_HDR(cmd, obj, len) \
    (((uint32_t)(len) << 16) | ((uint32_t)(obj) << 8) | (uint32_t)(cmd))

#define VIRGL_BUILD_ID 0xC0FFEE42u

/* ============================================================
 * Pipe targets (Gallium subset)
 * ============================================================ */
#define PIPE_BUFFER         0
#define PIPE_TEXTURE_1D     1
#define PIPE_TEXTURE_2D     2
#define PIPE_TEXTURE_3D     3
#define PIPE_TEXTURE_CUBE   4
#define PIPE_TEXTURE_RECT   5

/* ============================================================
 * Formats (use VIRGL_FORMAT_* ids; these match virgl_hw.h enum)
 * ============================================================ */
#define VIRGL_FORMAT_NONE                    0
#define VIRGL_FORMAT_B8G8R8A8_UNORM          1
#define VIRGL_FORMAT_B8G8R8X8_UNORM          2
#define VIRGL_FORMAT_Z16_UNORM               16
#define VIRGL_FORMAT_R32G32B32_FLOAT         30
#define VIRGL_FORMAT_R32G32B32A32_FLOAT      31

/* Aliases your code uses */
#define PIPE_FORMAT_NONE                     VIRGL_FORMAT_NONE
#define PIPE_FORMAT_B8G8R8A8_UNORM           VIRGL_FORMAT_B8G8R8A8_UNORM
#define PIPE_FORMAT_B8G8R8X8_UNORM           VIRGL_FORMAT_B8G8R8X8_UNORM
#define PIPE_FORMAT_Z16_UNORM                VIRGL_FORMAT_Z16_UNORM
#define PIPE_FORMAT_R32G32B32_FLOAT          VIRGL_FORMAT_R32G32B32_FLOAT
#define PIPE_FORMAT_R32G32B32A32_FLOAT       VIRGL_FORMAT_R32G32B32A32_FLOAT

/* ============================================================
 * Bind flags (virgl_hw.h)
 * ============================================================ */
#define VIRGL_BIND_DEPTH_STENCIL   (1u << 0)
#define VIRGL_BIND_RENDER_TARGET   (1u << 1)
#define VIRGL_BIND_SAMPLER_VIEW    (1u << 3)
#define VIRGL_BIND_VERTEX_BUFFER   (1u << 4)
#define VIRGL_BIND_INDEX_BUFFER    (1u << 5)
#define VIRGL_BIND_CONSTANT_BUFFER (1u << 6)
#define VIRGL_BIND_DISPLAY_TARGET  (1u << 7)

/* ============================================================
 * Primitive types
 * ============================================================ */
#define PIPE_PRIM_POINTS         0
#define PIPE_PRIM_LINES          1
#define PIPE_PRIM_LINE_STRIP     3
#define PIPE_PRIM_TRIANGLES      4
#define PIPE_PRIM_TRIANGLE_STRIP 5
#define PIPE_PRIM_TRIANGLE_FAN   6
#define PIPE_PRIM_QUADS          7

/* ============================================================
 * Shaders
 * ============================================================ */
#define PIPE_SHADER_VERTEX       0
#define PIPE_SHADER_FRAGMENT     1

/* ============================================================
 * Comparisons
 * ============================================================ */
#define PIPE_FUNC_NEVER    0
#define PIPE_FUNC_LESS     1
#define PIPE_FUNC_EQUAL    2
#define PIPE_FUNC_LEQUAL   3
#define PIPE_FUNC_GREATER  4
#define PIPE_FUNC_NOTEQUAL 5
#define PIPE_FUNC_GEQUAL   6
#define PIPE_FUNC_ALWAYS   7

/* ============================================================
 * Blend equations / factors (Gallium subset)
 * (These numeric values must match Gallium/Mesa.)
 * ============================================================ */
#define PIPE_BLEND_ADD              0
#define PIPE_BLEND_SUBTRACT         1
#define PIPE_BLEND_REVERSE_SUBTRACT 2
#define PIPE_BLEND_MIN              3
#define PIPE_BLEND_MAX              4

#define PIPE_BLENDFACTOR_ZERO               0
#define PIPE_BLENDFACTOR_ONE                1
#define PIPE_BLENDFACTOR_SRC_COLOR          2
#define PIPE_BLENDFACTOR_INV_SRC_COLOR      3
#define PIPE_BLENDFACTOR_SRC_ALPHA          4
#define PIPE_BLENDFACTOR_INV_SRC_ALPHA      5
#define PIPE_BLENDFACTOR_DST_ALPHA          6
#define PIPE_BLENDFACTOR_INV_DST_ALPHA      7
#define PIPE_BLENDFACTOR_DST_COLOR          8
#define PIPE_BLENDFACTOR_INV_DST_COLOR      9

/* ============================================================
 * Color write masks
 * ============================================================ */
#define PIPE_MASK_R  (1u << 0)
#define PIPE_MASK_G  (1u << 1)
#define PIPE_MASK_B  (1u << 2)
#define PIPE_MASK_A  (1u << 3)
#ifndef PIPE_MASK_RGBA
#define PIPE_MASK_RGBA 0xFu
#endif

/* ============================================================
 * Clear masks
 * ============================================================ */
#define PIPE_CLEAR_COLOR0   0x1u
#define PIPE_CLEAR_DEPTH    0x2u
#define PIPE_CLEAR_STENCIL  0x4u
#define VIRGL_FORMAT_NONE                    0
/* ============================================================
 * Driver context state struct
 * ============================================================ */
typedef struct {
    bool     initialized;
    uint32_t ctx_id;

    uint32_t* cmd_buf;
    uint32_t  cmd_buf_phys;
    uint32_t  cmd_buf_size;
    uint32_t  cmd_pos;

    uint32_t  next_res_id;

    uint32_t  color_surface_handle;
    uint32_t  depth_surface_handle;
    uint32_t  blend_handle;
    uint32_t  rasterizer_handle;
    uint32_t  dsa_handle;
    uint32_t  vs_handle;
    uint32_t  fs_handle;
    uint32_t  ve_handle;

    uint32_t  fb_res_id;
    uint32_t  fb_width, fb_height;
    uint32_t  depth_res_id;
    uint32_t  display_res_id;

    uint32_t  vbo_res_id;
    uint32_t  vbo_size;
} virgl_ctx_t;

/* ============================================================
 * Public API
 * ============================================================ */
bool virgl_init(void);
bool virgl_available(void);
bool virgl_setup_framebuffer(uint16_t width, uint16_t height);
virgl_ctx_t* virgl_get_ctx(void);

void virgl_cmd_begin(void);

void virgl_cmd_clear(uint32_t buffers,
                     float r, float g, float b, float a,
                     double depth, uint32_t stencil);

uint32_t virgl_upload_vertices(const float* data, uint32_t num_floats);
void virgl_cmd_set_vertex_buffer(uint32_t stride, uint32_t offset);
void virgl_cmd_draw(uint32_t prim_mode, uint32_t start, uint32_t count);

void virgl_cmd_set_constant_buffer(uint32_t shader_type,
                                   const float* data, uint32_t num_floats);

bool virgl_cmd_submit(void);
void virgl_present(void);
void virgl_shutdown(void);

uint32_t* virgl_get_display_backing(void);
bool virgl_setup_pipeline_state(void);
void virgl_cmd_set_viewport(uint32_t w, uint32_t h);

void virgl_cmd_disable_depth(void);
void virgl_set_depth_test(uint32_t enabled);
#endif /* VIRGL_H */
