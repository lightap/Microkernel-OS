#ifndef VIRGL_H
#define VIRGL_H

#include "types.h"
#include "virtio_gpu.h"

/*
 * Virgl 3D Protocol — GPU-accelerated rendering over VirtIO-GPU
 *
 * HOW THIS WORKS:
 *
 *   Your minigl does this on the CPU:
 *     vertex transform → clipping → rasterization → z-test → shading → pixels
 *
 *   Virgl offloads all of that to the HOST GPU. Instead of computing pixels,
 *   you build a COMMAND BUFFER describing what to draw (vertices, state,
 *   draw calls) and submit it via virtio-gpu. The host's virglrenderer
 *   translates it into real OpenGL/Vulkan calls on the host GPU.
 *
 *   Guest (your OS)                     Host (QEMU + virglrenderer)
 *   ─────────────────                   ─────────────────────────────
 *   virgl_cmd_clear()           ──→     glClear()
 *   virgl_cmd_set_viewport()    ──→     glViewport()
 *   virgl_cmd_draw_vbo()        ──→     glDrawArrays()
 *   virgl_cmd_present()         ──→     
 *
 * QEMU COMMAND LINE (required):
 *   qemu-system-i386 -device virtio-gpu-gl-pci  (NOT virtio-gpu-pci!)
 *   Also needs: -display gtk,gl=on  OR  -display sdl,gl=on
 */

/* ===== VirtIO-GPU 3D Commands (in addition to 2D ones) ===== */
#define VIRTIO_GPU_CMD_CTX_CREATE              0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY             0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE     0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE     0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D      0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D     0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D   0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D               0x0207

/* ===== 3D Command Structures ===== */

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t nlen;         /* Length of name string (including NUL) */
    uint32_t context_init; /* Context initialization flags (0 for standard virgl) */
    char     debug_name[64];
} __attribute__((packed)) virtio_gpu_ctx_create_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t target;       /* PIPE_TEXTURE_2D, PIPE_BUFFER, etc. */
    uint32_t format;       /* VIRGL_FORMAT_* */
    uint32_t bind;         /* VIRGL_BIND_* */
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t padding;      /* Must be present — QEMU expects 72-byte struct */
} __attribute__((packed)) virtio_gpu_resource_create_3d_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_ctx_resource_t;



typedef struct __attribute__((packed)) virtio_gpu_cmd_submit {
    virtio_gpu_ctrl_hdr_t hdr;  /* 24 */
    uint32_t size;              /* in dwords */
} virtio_gpu_cmd_submit_t;      /* MUST be 28 */
_Static_assert(sizeof(virtio_gpu_ctrl_hdr_t) == 24, "ctrl_hdr must be 24");
_Static_assert(sizeof(virtio_gpu_cmd_submit_t) == 28, "cmd_submit must be 28");



/* ===== Virgl Wire Protocol — Command Buffer Format ===== */
/*
 * The command buffer is a stream of uint32_t words:
 *   [header] [payload...]  [header] [payload...] ...
 *
 * Header format: (length << 16) | (object_type << 8) | command
 *   length = number of uint32_t PAYLOAD words (not including header)
 */

/* Virgl commands (VIRGL_CCMD_*) */
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

/* Object types (for CREATE_OBJECT) */
#define VIRGL_OBJECT_BLEND                  1
#define VIRGL_OBJECT_RASTERIZER             2
#define VIRGL_OBJECT_DSA                    3   /* Depth-Stencil-Alpha */
#define VIRGL_OBJECT_SHADER                 4
#define VIRGL_OBJECT_VERTEX_ELEMENTS        5
#define VIRGL_OBJECT_SAMPLER_VIEW           6
#define VIRGL_OBJECT_SAMPLER_STATE          7
#define VIRGL_OBJECT_SURFACE                8
#define VIRGL_OBJECT_QUERY                  9
#define VIRGL_OBJECT_STREAMOUT_TARGET       10

/* Pipe targets (Gallium3D) */
#define PIPE_BUFFER         0
#define PIPE_TEXTURE_1D     1
#define PIPE_TEXTURE_2D     2
#define PIPE_TEXTURE_3D     3
#define PIPE_TEXTURE_CUBE   4
#define PIPE_TEXTURE_RECT   5

/* Virgl formats (subset — matches Gallium PIPE_FORMAT) */
#define VIRGL_FORMAT_B8G8R8A8_UNORM    1
#define VIRGL_FORMAT_B8G8R8X8_UNORM    2
#define VIRGL_FORMAT_R8G8B8A8_UNORM    4
#define VIRGL_FORMAT_Z24_UNORM_S8_UINT 19  /* Depth+stencil */
#define VIRGL_FORMAT_Z32_FLOAT         25
#define VIRGL_FORMAT_R32G32B32A32_FLOAT 31
#define VIRGL_FORMAT_R32G32B32_FLOAT   30
#define VIRGL_FORMAT_Z24X8_UNORM       33
#define VIRGL_FORMAT_Z16_UNORM         16
#define VIRGL_FORMAT_R32_FLOAT         28

/* Bind flags */
#define VIRGL_BIND_DEPTH_STENCIL   (1 << 0)
#define VIRGL_BIND_RENDER_TARGET   (1 << 1)
#define VIRGL_BIND_SAMPLER_VIEW    (1 << 3)
#define VIRGL_BIND_VERTEX_BUFFER   (1 << 4)
#define VIRGL_BIND_INDEX_BUFFER    (1 << 5)
#define VIRGL_BIND_CONSTANT_BUFFER (1 << 6)

/* Primitive types (Gallium PIPE_PRIM_*) */
#define PIPE_PRIM_POINTS         0
#define PIPE_PRIM_LINES          1
#define PIPE_PRIM_LINE_STRIP     3
#define PIPE_PRIM_TRIANGLES      4
#define PIPE_PRIM_TRIANGLE_STRIP 5
#define PIPE_PRIM_TRIANGLE_FAN   6
#define PIPE_PRIM_QUADS          7



/* TGSI token types (for shader encoding) */
#define TGSI_TOKEN_TYPE_DECLARATION  1
#define TGSI_TOKEN_TYPE_INSTRUCTION  2
#define TGSI_TOKEN_TYPE_PROPERTY     3


/* Face culling modes */
#define PIPE_FACE_NONE  0
#define PIPE_FACE_FRONT 1
#define PIPE_FACE_BACK  2
#define PIPE_FACE_BOTH  3

/* Polygon fill modes */
#define PIPE_POLYGON_MODE_FILL  0
#define PIPE_POLYGON_MODE_LINE  1
#define PIPE_POLYGON_MODE_POINT 2

/* Blend functions */
#define PIPE_BLEND_ADD              0
#define PIPE_BLEND_SUBTRACT         1
#define PIPE_BLEND_REVERSE_SUBTRACT 2
#define PIPE_BLEND_MIN              3
#define PIPE_BLEND_MAX              4
#define PIPE_MASK_RGBA              0xF    

/* Gallium / Virgl blend factors (protocol values) */
#define PIPE_BLENDFACTOR_ZERO               0
#define PIPE_BLENDFACTOR_ONE                1
#define PIPE_BLENDFACTOR_SRC_COLOR          3
#define PIPE_BLENDFACTOR_INV_SRC_COLOR      4
#define PIPE_BLENDFACTOR_SRC_ALPHA          5
#define PIPE_BLENDFACTOR_INV_SRC_ALPHA      6
#define PIPE_BLENDFACTOR_DST_COLOR          7
#define PIPE_BLENDFACTOR_INV_DST_COLOR      8
#define PIPE_BLENDFACTOR_DST_ALPHA          9
#define PIPE_BLENDFACTOR_INV_DST_ALPHA      10




/* Comparison functions */
#define PIPE_FUNC_NEVER    0
#define PIPE_FUNC_LESS     1
#define PIPE_FUNC_EQUAL    2
#define PIPE_FUNC_LEQUAL   3
#define PIPE_FUNC_GREATER  4
#define PIPE_FUNC_NOTEQUAL 5
#define PIPE_FUNC_GEQUAL   6
#define PIPE_FUNC_ALWAYS   7

/* Color write mask */
#define PIPE_MASK_R  (1 << 0)
#define PIPE_MASK_G  (1 << 1)
#define PIPE_MASK_B  (1 << 2)
#define PIPE_MASK_A  (1 << 3)
#ifndef PIPE_MASK_RGBA
#define PIPE_MASK_RGBA 0xF
#endif

#define PIPE_CLEAR_COLOR0   0x1
#define PIPE_CLEAR_DEPTH    0x2
#define PIPE_CLEAR_STENCIL  0x4


#define PIPE_SHADER_VERTEX   0
#define PIPE_SHADER_FRAGMENT 1




    /* payload_len is number of DWORDS AFTER the header */
#define VIRGL_CMD_HDR(cmd, obj, len) \
    (((len) << 16) | ((obj) << 8) | (cmd))


/* ===== Virgl Context (driver-side state) ===== */
typedef struct {
    bool     initialized;
    uint32_t ctx_id;            /* virgl rendering context */
    /* Command buffer being built */
    uint32_t* cmd_buf;          /* Command buffer memory (phys == virt) */
    uint32_t  cmd_buf_phys;
    uint32_t  cmd_buf_size;     /* Allocated size in bytes */
    uint32_t  cmd_pos;          /* Current write position (in uint32_t words) */
    /* Resource IDs (we allocate these incrementally) */
    uint32_t  next_res_id;
    /* Pre-created GPU objects (handles) */
    uint32_t  color_surface_handle;
    uint32_t  depth_surface_handle;
    uint32_t  blend_handle;
    uint32_t  rasterizer_handle;
    uint32_t  dsa_handle;        /* Depth-stencil-alpha state */
    uint32_t  vs_handle;         /* Vertex shader */
    uint32_t  fs_handle;         /* Fragment shader */
    uint32_t  ve_handle;         /* Vertex elements */
    /* Framebuffer resource */
    uint32_t  fb_res_id;
    uint32_t  fb_width, fb_height;
    uint32_t  depth_res_id;
    uint32_t  display_res_id;    /* ADD THIS - 2D scanout resource */
    /* Vertex buffer resource */
    uint32_t  vbo_res_id;
    uint32_t  vbo_size;          /* Current allocation in bytes */
} virgl_ctx_t;
/* ===== Public API ===== */

/* Initialize virgl: negotiate 3D feature, create context.
 * Returns true if host supports virgl (requires virtio-gpu-gl-pci). */
bool virgl_init(void);

/* Check if virgl 3D is available */
bool virgl_available(void);

/* Set up a rendering framebuffer at the given resolution.
 * Creates color + depth resources and sets up scanout. */
bool virgl_setup_framebuffer(uint16_t width, uint16_t height);

/* Get the virgl context for advanced usage */
virgl_ctx_t* virgl_get_ctx(void);

/* ---- Command buffer API ---- */

/* Begin building a new command buffer */
void virgl_cmd_begin(void);

/* Append a clear command (like glClear) */
void virgl_cmd_clear(uint32_t buffers,
                     float r, float g, float b, float a,
                     float depth, uint32_t stencil);

/* Set the viewport */
//void virgl_cmd_set_viewport(float x, float y, float width, float height,
  //                          float near_val, float far_val);

/* Upload vertex data to GPU buffer, returns offset */
uint32_t virgl_upload_vertices(const float* data, uint32_t num_floats);

/* Set vertex buffer binding */
void virgl_cmd_set_vertex_buffer(uint32_t stride, uint32_t offset);

/* Draw primitives */
void virgl_cmd_draw(uint32_t prim_mode, uint32_t start, uint32_t count);

/* Set a uniform/constant buffer (model-view-projection matrix etc.) */
void virgl_cmd_set_constant_buffer(uint32_t shader_type,
                                   const float* data, uint32_t num_floats);

/* Submit the command buffer to the GPU and wait for completion */
bool virgl_cmd_submit(void);

/* Transfer the rendered framebuffer to the display (like SwapBuffers) */
void virgl_present(void);

/* Shut down virgl */
void virgl_shutdown(void);

uint32_t* virgl_get_display_backing(void);

bool virgl_setup_pipeline_state(void); 

void virgl_cmd_set_viewport(uint32_t w, uint32_t h);

#endif
