#ifndef VIRGL_PIPELINE_H
#define VIRGL_PIPELINE_H

#include "types.h"
#include "virgl.h"

/*
 * Virgl Pipeline State Encoding
 *
 * This implements the ~600 lines of state object creation that Mesa's
 * virgl Gallium driver does in virgl_encode.c. It creates and binds:
 *
 *   1. Blend state         (VIRGL_OBJECT_BLEND)
 *   2. Rasterizer state    (VIRGL_OBJECT_RASTERIZER)
 *   3. Depth-stencil-alpha (VIRGL_OBJECT_DSA)
 *   4. Vertex elements     (VIRGL_OBJECT_VERTEX_ELEMENTS)
 *   5. Surface objects     (VIRGL_OBJECT_SURFACE) — color + depth
 *   6. Shaders             (VIRGL_OBJECT_SHADER) — vertex + fragment (TGSI text)
 *   7. Framebuffer state   (SET_FRAMEBUFFER_STATE)
 *
 * Wire format reference: virglrenderer vrend_decode.c + Mesa virgl_hw.h
 */

/* ===== Gallium pipe_blend_func values ===== */
#define PIPE_BLEND_ADD              0
#define PIPE_BLEND_SUBTRACT         1
#define PIPE_BLEND_REVERSE_SUBTRACT 2
#define PIPE_BLEND_MIN              3
#define PIPE_BLEND_MAX              4

/* ===== Gallium pipe_blendfactor values ===== */
//#define PIPE_BLENDFACTOR_ONE                 0x01
//#define PIPE_BLENDFACTOR_SRC_COLOR           0x02
//#define PIPE_BLENDFACTOR_SRC_ALPHA           0x03
#define PIPE_BLENDFACTOR_DST_ALPHA           0x04
#define PIPE_BLENDFACTOR_DST_COLOR           0x05
#define PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE  0x06
#define PIPE_BLENDFACTOR_CONST_COLOR         0x07
#define PIPE_BLENDFACTOR_CONST_ALPHA         0x08
//#define PIPE_BLENDFACTOR_ZERO                0x11
//#define PIPE_BLENDFACTOR_INV_SRC_COLOR       0x12
//#define PIPE_BLENDFACTOR_INV_SRC_ALPHA       0x13
#define PIPE_BLENDFACTOR_INV_DST_ALPHA       0x14
#define PIPE_BLENDFACTOR_INV_DST_COLOR       0x15




/* ===== Gallium pipe_func (comparison functions) ===== */
#define PIPE_FUNC_NEVER    0
#define PIPE_FUNC_LESS     1
#define PIPE_FUNC_EQUAL    2
#define PIPE_FUNC_LEQUAL   3
#define PIPE_FUNC_GREATER  4
#define PIPE_FUNC_NOTEQUAL 5
#define PIPE_FUNC_GEQUAL   6
#define PIPE_FUNC_ALWAYS   7

/* ===== Gallium pipe_stencil_op ===== */
#define PIPE_STENCIL_OP_KEEP     0
#define PIPE_STENCIL_OP_ZERO     1
#define PIPE_STENCIL_OP_REPLACE  2
#define PIPE_STENCIL_OP_INCR     3
#define PIPE_STENCIL_OP_DECR     4
#define PIPE_STENCIL_OP_INCR_WRAP 5
#define PIPE_STENCIL_OP_DECR_WRAP 6
#define PIPE_STENCIL_OP_INVERT   7

/* ===== Gallium fill/cull modes ===== */
#define PIPE_FACE_NONE           0
#define PIPE_FACE_FRONT          1
#define PIPE_FACE_BACK           2
#define PIPE_FACE_FRONT_AND_BACK 3

#define PIPE_POLYGON_MODE_FILL   0
#define PIPE_POLYGON_MODE_LINE   1
#define PIPE_POLYGON_MODE_POINT  2

/* ===== Color mask ===== */
#define PIPE_MASK_R  0x1
#define PIPE_MASK_G  0x2
#define PIPE_MASK_B  0x4
#define PIPE_MASK_A  0x8
#define PIPE_MASK_RGBA 0xF

/* ===== Clear flags ===== */
#define PIPE_CLEAR_COLOR   0x1
#define PIPE_CLEAR_DEPTH   0x2
#define PIPE_CLEAR_STENCIL 0x4

/* ===== Public API ===== */

/* Create all GPU pipeline state objects and bind them.
 * Must be called AFTER virgl_init() and virgl_setup_framebuffer().
 * Returns true on success. */
bool virgl_pipeline_setup(void);

/* Re-bind the DSA state (e.g., after toggling depth test) */
void virgl_pipeline_set_depth_test(bool enable);

#endif /* VIRGL_PIPELINE_H */
