#include "virgl_pipeline.h"
#include "virgl.h"
#include "serial.h"
#include "heap.h"

/*
 * Virgl Pipeline State Encoding
 *
 * Ported from Mesa's virgl Gallium driver (virgl_encode.c) and verified
 * against virglrenderer's decoder (vrend_decode.c).
 *
 * Wire format:
 *   Every command is a stream of uint32_t words.
 *   First word = header: (payload_len << 16) | (obj_type << 8) | command
 *   Followed by payload_len words of payload.
 *
 * For CREATE_OBJECT commands, the first payload word is always the handle.
 * For BIND_OBJECT commands, the only payload word is the handle.
 *
 * Reference files:
 *   Mesa:           src/gallium/drivers/virgl/virgl_encode.c
 *                   src/gallium/drivers/virgl/virgl_hw.h
 *   virglrenderer:  src/vrend_decode.c
 */

/* ===== Handle allocator (unique per state object) ===== */
static uint32_t next_handle = 10;  /* Start above 0 to avoid confusion with NULL */
static uint32_t alloc_handle(void) { return next_handle++; }

/* ===== Float-to-uint32 reinterpret ===== */
static inline uint32_t f2u(float f) {
    union { float f; uint32_t u; } x;
    x.f = f;
    return x.u;
}

/* ===== Command buffer emission (uses virgl.c's cmd_buf) ===== */
static virgl_ctx_t* ctx;

static inline void emit(uint32_t word) {
    if (ctx->cmd_pos < ctx->cmd_buf_size / 4) {
        ctx->cmd_buf[ctx->cmd_pos++] = word;
    } else {
        serial_printf("virgl_pipeline: command buffer overflow at pos %u!\n", ctx->cmd_pos);
    }
}

/*
 * ================================================================
 *  1. BLEND STATE (VIRGL_OBJECT_BLEND)
 * ================================================================
 *
 * Wire format (from vrend_decode_create_blend):
 *   word 0: handle
 *   word 1: S0 = (independent_blend_enable)
 *                | (logicop_enable << 1)
 *                | (dither << 2)
 *                | (alpha_to_coverage << 3)
 *                | (alpha_to_one << 4)
 *   word 2: S1 = logicop_func
 *   words 3+: per-RT blend state (1 RT if !independent, else 8):
 *     S2 = (blend_enable)
 *        | (rgb_func << 1)        [3 bits]
 *        | (rgb_src_factor << 4)  [5 bits]
 *        | (rgb_dst_factor << 9)  [5 bits]
 *        | (alpha_func << 14)     [3 bits]
 *        | (alpha_src_factor << 17) [5 bits]
 *        | (alpha_dst_factor << 22) [5 bits]
 *        | (colormask << 27)      [4 bits]
 */
static uint32_t create_blend_state(bool blend_enable) {
    uint32_t handle = alloc_handle();

    /* BLEND object expects: handle + s0 + s1 + s2[8]  => payload_len = 11 */
    const uint32_t payload_len = 1 + 1 + 1 + 8;

    emit(VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_BLEND, payload_len));
    emit(handle);

    /* s0: independent_blend_enable=1 (because we are sending 8 RT entries)
       other bits 0: logicop=0, dither=0, alpha_to_coverage=0, alpha_to_one=0 */
    emit(1);

    /* s1: logicop_func (ignored because logicop_enable=0) */
    emit(0);

    uint32_t s2;
    if (blend_enable) {
        /* src*srcA + dst*(1-srcA) */
        s2 =
            (1) |
            (PIPE_BLEND_ADD << 1) |
            (PIPE_BLENDFACTOR_SRC_ALPHA << 4) |
            (PIPE_BLENDFACTOR_INV_SRC_ALPHA << 9) |
            (PIPE_BLEND_ADD << 14) |
            (PIPE_BLENDFACTOR_ONE << 17) |
            (PIPE_BLENDFACTOR_INV_SRC_ALPHA << 22) |
            (PIPE_MASK_RGBA << 27);
    } else {
        /* no blending, write all channels */
        s2 =
            (0) |
            (PIPE_BLEND_ADD << 1) |
            (PIPE_BLENDFACTOR_ONE << 4) |
            (PIPE_BLENDFACTOR_ZERO << 9) |
            (PIPE_BLEND_ADD << 14) |
            (PIPE_BLENDFACTOR_ONE << 17) |
            (PIPE_BLENDFACTOR_ZERO << 22) |
            (PIPE_MASK_RGBA << 27);
    }

    /* emit S2 for all 8 render targets */
    for (int i = 0; i < 8; i++)
        emit(s2);

    return handle;
}


/*
 * ================================================================
 *  2. RASTERIZER STATE (VIRGL_OBJECT_RASTERIZER)
 * ================================================================
 *
 * Wire format (from vrend_decode_create_rasterizer):
 *   word 0: handle
 *   word 1: S0 (bitfield — see below)
 *   word 2: point_size (float)
 *   word 3: sprite_coord_enable
 *   word 4: S3 = line_width (float)  -- vrend_decode uses (tmp & 0x7fff...8) trick
 *   word 5: offset_units (float)
 *   word 6: offset_scale (float)
 *   word 7: offset_clamp (float)
 *
 * S0 bitfield:
 *   bit 0:      flatshade
 *   bit 1:      depth_clip_near
 *   bit 2:      depth_clip_far
 *   bit 3:      rasterizer_discard
 *   bit 4:      flatshade_first
 *   bit 5:      light_twoside
 *   bit 6:      sprite_coord_mode
 *   bit 7:      point_quad_rasterization
 *   bits 8-9:   cull_face (PIPE_FACE_*)
 *   bits 10-11: fill_front (PIPE_POLYGON_MODE_*)
 *   bits 12-13: fill_back (PIPE_POLYGON_MODE_*)
 *   bit 14:     scissor
 *   bit 15:     front_ccw (1 = counter-clockwise is front)
 *   bit 16:     clamp_vertex_color
 *   bit 17:     clamp_fragment_color
 *   bit 18:     offset_line
 *   bit 19:     offset_point
 *   bit 20:     offset_tri
 *   bit 21:     poly_smooth
 *   bit 22:     poly_stipple_enable
 *   bit 23:     point_smooth
 *   bit 24:     point_size_per_vertex
 *   bit 25:     multisample
 *   bit 26:     line_smooth
 *   bit 27:     line_stipple_enable
 *   bit 28:     line_last_pixel
 *   bit 29:     half_pixel_center
 *   bit 30:     bottom_edge_rule
 *   bit 31:     force_persample_interp
 */
static uint32_t create_rasterizer_state(void) {
    uint32_t handle = alloc_handle();
    uint32_t payload_len = 8;  /* handle + S0 + point_size + sprite_coord + line_width + 3 offsets */

    emit(VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_RASTERIZER, payload_len));
    emit(handle);

    /* S0: sensible defaults for a basic 3D renderer */
    uint32_t s0 = 0;
    s0 |= (0 << 0);   /* flatshade = false (smooth shading) */
    s0 |= (1 << 1);   /* depth_clip_near = true */
    s0 |= (1 << 2);   /* depth_clip_far = true */
    s0 |= (0 << 3);   /* rasterizer_discard = false */
    s0 |= (0 << 4);   /* flatshade_first = false */
    s0 |= (0 << 5);   /* light_twoside = false */
    /* cull_face: PIPE_FACE_NONE=0 (no culling for now) */
    s0 |= (PIPE_FACE_NONE << 8);
    /* fill modes: PIPE_POLYGON_MODE_FILL=0 */
    s0 |= (PIPE_POLYGON_MODE_FILL << 10);  /* fill_front */
    s0 |= (PIPE_POLYGON_MODE_FILL << 12);  /* fill_back */
    s0 |= (0 << 14);  /* scissor = false */
    s0 |= (1 << 15);  /* front_ccw = true (OpenGL convention) */
    s0 |= (0 << 16);  /* clamp_vertex_color = false */
    s0 |= (0 << 17);  /* clamp_fragment_color = false */
    s0 |= (1 << 29);  /* half_pixel_center = true */
    s0 |= (0 << 30);  /* bottom_edge_rule = false */
    emit(s0);

    emit(f2u(1.0f));  /* point_size */
    emit(0);           /* sprite_coord_enable */
    emit(f2u(1.0f));  /* line_width */
    emit(f2u(0.0f));  /* offset_units */
    emit(f2u(0.0f));  /* offset_scale */
    emit(f2u(0.0f));  /* offset_clamp */

    serial_printf("virgl_pipeline: created rasterizer state handle=%u\n", handle);
    return handle;
}

/*
 * ================================================================
 *  3. DEPTH-STENCIL-ALPHA STATE (VIRGL_OBJECT_DSA)
 * ================================================================
 *
 * Wire format (from vrend_decode_create_dsa):
 *   word 0: handle
 *   word 1: S0 = (depth_enabled)
 *              | (depth_writemask << 1)
 *              | (depth_func << 2)       [3 bits]
 *              | (alpha_enabled << 8)
 *              | (alpha_func << 9)       [3 bits]
 *   word 2: S1 = stencil[0]:
 *              (enabled)
 *              | (func << 1)             [3 bits]
 *              | (fail_op << 4)          [3 bits]
 *              | (zpass_op << 7)         [3 bits]
 *              | (zfail_op << 10)        [3 bits]
 *              | (valuemask << 13)       [8 bits]
 *              | (writemask << 21)       [8 bits]
 *   word 3: S2 = stencil[1] (same format)
 *   word 4: alpha_ref_value (float)
 */
static uint32_t create_dsa_state(bool depth_test, bool depth_write) {
    uint32_t handle = alloc_handle();
    uint32_t payload_len = 5;  /* handle + S0 + S1 + S2 + alpha_ref */

    emit(VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_DSA, payload_len));
    emit(handle);

    /* S0: depth + alpha settings */
    uint32_t s0 = 0;
    if (depth_test) {
        s0 |= (1 << 0);              /* depth_enabled */
        s0 |= (depth_write << 1);    /* depth_writemask */
        s0 |= (PIPE_FUNC_LESS << 2); /* depth_func = LESS */
    }
    /* alpha_enabled = 0 (no alpha test), alpha_func = 0 */
    emit(s0);

    /* S1: stencil[0] — disabled */
    emit(0);

    /* S2: stencil[1] — disabled */
    emit(0);

    /* Alpha reference value */
    emit(f2u(0.0f));

    serial_printf("virgl_pipeline: created DSA state handle=%u depth=%d write=%d\n",
                  handle, depth_test, depth_write);
    return handle;
}

/*
 * ================================================================
 *  4. VERTEX ELEMENTS (VIRGL_OBJECT_VERTEX_ELEMENTS)
 * ================================================================
 *
 * Wire format (from vrend_decode_create_ve):
 *   word 0: handle
 *   Then groups of 4 words per element:
 *     word N+0: src_offset
 *     word N+1: instance_divisor
 *     word N+2: vertex_buffer_index
 *     word N+3: src_format (VIRGL_FORMAT_*)
 *
 *   Number of elements = (payload_len - 1) / 4
 *
 * Our vertex format is: position(3 floats) + color(4 floats) = 28 bytes
 *   Element 0: position at offset 0,  format = R32G32B32_FLOAT
 *   Element 1: color    at offset 12, format = R32G32B32A32_FLOAT
 */
static uint32_t create_vertex_elements(void) {
    uint32_t handle = alloc_handle();
    uint32_t num_elements = 2;
    uint32_t payload_len = 1 + (num_elements * 4);  /* handle + 4 words per element */

    emit(VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, payload_len));
    emit(handle);

    /* Element 0: position (vec3 at offset 0) */
    emit(0);                           /* src_offset = 0 bytes */
    emit(0);                           /* instance_divisor = 0 */
    emit(0);                           /* vertex_buffer_index = 0 */
    emit(VIRGL_FORMAT_R32G32B32_FLOAT); /* format */

    /* Element 1: color (vec4 at offset 12) */
    emit(12);                            /* src_offset = 12 bytes (after 3 floats) */
    emit(0);                             /* instance_divisor = 0 */
    emit(0);                             /* vertex_buffer_index = 0 */
    emit(VIRGL_FORMAT_R32G32B32A32_FLOAT); /* format */

    serial_printf("virgl_pipeline: created vertex elements handle=%u (%u elements)\n",
                  handle, num_elements);
    return handle;
}

/*
 * ================================================================
 *  5. SURFACE OBJECTS (VIRGL_OBJECT_SURFACE)
 * ================================================================
 *
 * Wire format (from vrend_decode_create_surface):
 *   word 0: handle
 *   word 1: resource_id
 *   word 2: format (VIRGL_FORMAT_*)
 *   word 3: val0 — for textures: level; for buffers: first_element
 *   word 4: val1 — for textures: (first_layer) | (last_layer << 16);
 *                   for buffers: last_element
 *
 * A surface is a "view" of a resource used as a framebuffer attachment.
 * We create one for the color buffer and one for the depth buffer.
 */
static uint32_t create_surface(uint32_t resource_id, uint32_t format) {
    uint32_t handle = alloc_handle();
    uint32_t payload_len = 5; // Handle + res_id + format + val0 + val1

    emit(VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, payload_len));
    emit(handle);
    emit(resource_id);
    emit(format);
    emit(0);     /* val0: level = 0 */
    emit(0);     /* val1: first_layer=0, last_layer=0 */

    return handle;
}

/*
 * ================================================================
 *  6. SHADER OBJECTS (VIRGL_OBJECT_SHADER)
 * ================================================================
 *
 * Wire format (from vrend_decode_create_shader):
 *   word 0: handle
 *   word 1: type (PIPE_SHADER_VERTEX=0 or PIPE_SHADER_FRAGMENT=1)
 *   word 2: num_tokens (used for multi-packet shaders; we set to 0)
 *   word 3: offlen = (offset << 8) | num_stream_outputs
 *            For single-packet, offset=0, num_so=0, so offlen=0
 *   words 4+: TGSI text as null-terminated string packed into uint32_t words
 *             (zero-padded to 4-byte boundary)
 *
 * TGSI text format (parsed by tgsi_text.c in Mesa):
 *   "VERT\n" or "FRAG\n"
 *   "DCL IN[0]\n"
 *   "DCL OUT[0], POSITION\n"
 *   "  0: MOV OUT[0], IN[0]\n"
 *   "  1: END\n"
 *
 * Our vertex shader:
 *   - Takes position (IN[0], vec3) and color (IN[1], vec4)
 *   - Multiplies position by MVP matrix stored in CONST[0..3]
 *   - Passes color through to fragment shader
 *
 * Our fragment shader:
 *   - Takes interpolated color (IN[0])
 *   - Outputs it as the pixel color
 */

/* Pack a string into uint32_t words, zero-padded */
static uint32_t emit_string(const char* str) {
    uint32_t len = 0;
    while (str[len]) len++;
    len++;  /* Include null terminator */

    /* Pad to 4-byte boundary */
    uint32_t padded = (len + 3) & ~3;
    uint32_t num_words = padded / 4;

    const uint8_t* src = (const uint8_t*)str;
    for (uint32_t i = 0; i < num_words; i++) {
        uint32_t word = 0;
        for (int b = 0; b < 4; b++) {
            uint32_t offset = i * 4 + b;
            if (offset < len)
                word |= ((uint32_t)src[offset]) << (b * 8);
            /* else word bits stay 0 (zero-padded) */
        }
        emit(word);
    }

    return num_words;
}

/* Vertex shader: MVP transform + color pass-through
 *
 * TGSI text equivalent of:
 *   gl_Position = MVP * vec4(in_position, 1.0);
 *   v_color = in_color;
 *
 * MVP is stored as 4 row vectors in CONST[0..3].
 * We compute each component with DP4:
 *   OUT[0].x = dot(CONST[0], vec4(IN[0].xyz, 1.0))
 *   OUT[0].y = dot(CONST[1], vec4(IN[0].xyz, 1.0))
 *   etc.
 */
static const char vs_tgsi_text[] =
    "VERT\n"
    "DCL IN[0]\n"
    "DCL IN[1]\n"
    "DCL OUT[0], POSITION\n"
    "DCL OUT[1], GENERIC[0]\n"
    "DCL CONST[0..3]\n"
    "DCL TEMP[0]\n"
    "IMM[0] FLT32 {    1.0000,     0.0000,     0.0000,     0.0000}\n"
    "  0: MOV TEMP[0], IN[0]\n"
    "  1: MOV TEMP[0].w, IMM[0].xxxx\n"
    "  2: DP4 OUT[0].x, CONST[0], TEMP[0]\n"
    "  3: DP4 OUT[0].y, CONST[1], TEMP[0]\n"
    "  4: DP4 OUT[0].z, CONST[2], TEMP[0]\n"
    "  5: DP4 OUT[0].w, CONST[3], TEMP[0]\n"
    "  6: MOV OUT[1], IN[1]\n"
    "  7: END\n";

/* Fragment shader: output interpolated color
 *
 * TGSI text equivalent of:
 *   out_color = v_color;
 */
static const char fs_tgsi_text[] =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "  0: MOV OUT[0], IN[0]\n"
    "  1: END\n";

static uint32_t create_shader(uint32_t type, const char* tgsi_text) {
    uint32_t handle = alloc_handle();

    /* Calculate text length in words (null-terminated, zero-padded) */
    uint32_t text_len = 0;
    while (tgsi_text[text_len]) text_len++;
    text_len++;  /* null terminator */
    uint32_t text_words = (text_len + 3) / 4;

    /* Payload: handle + type + num_tokens + offlen + text_words */
    uint32_t payload_len = 4 + text_words;

    emit(VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER, payload_len));
    emit(handle);
    emit(type);         /* PIPE_SHADER_VERTEX=0 or PIPE_SHADER_FRAGMENT=1 */
    emit(0);            /* num_tokens: 0 = single packet, full text follows */
    emit(0);            /* offlen: offset=0, num_stream_outputs=0 */

    /* Emit TGSI text packed into uint32_t words */
    emit_string(tgsi_text);

    serial_printf("virgl_pipeline: created %s shader handle=%u (%u bytes TGSI)\n",
                  type == PIPE_SHADER_VERTEX ? "vertex" : "fragment",
                  handle, text_len - 1);
    return handle;
}

/*
 * ================================================================
 *  7. BIND COMMANDS
 * ================================================================
 *
 * Wire format (from vrend_decode_bind_object):
 *   Header: VIRGL_CMD_HDR(BIND_OBJECT, obj_type, 1)
 *   word 0: handle (0 = unbind)
 *
 * Simple: just the header with object type and the handle.
 */
static void bind_object(uint32_t obj_type, uint32_t handle) {
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_BIND_OBJECT, obj_type, 1));
    emit(handle);
}

/*
 * ================================================================
 *  8. SET FRAMEBUFFER STATE
 * ================================================================
 *
 * Wire format (from vrend_decode_set_framebuffer_state):
 *   word 0: nr_cbufs (number of color buffer attachments)
 *   word 1: zsurf_handle (depth/stencil surface handle, 0 = none)
 *   words 2+: cbuf_handles[0..nr_cbufs-1]
 *
 * Payload length = 2 + nr_cbufs
 */
static void set_framebuffer_state(uint32_t color_surface, uint32_t depth_surface) {
    uint32_t nr_cbufs = 1;
    uint32_t payload_len = 2 + nr_cbufs;

    emit(VIRGL_CMD_HDR(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, payload_len));
    emit(nr_cbufs);
    emit(depth_surface);    /* zsurf_handle */
    emit(color_surface);    /* cbufs[0] */
}

/*
 * ================================================================
 *  9. SET VIEWPORT (corrected format)
 * ================================================================
 *
 * Wire format (from vrend_decode_set_viewport_state):
 *   word 0: start_slot
 *   Then groups of 6 floats per viewport:
 *     scale_x, scale_y, scale_z, translate_x, translate_y, translate_z
 *
 *   Number of viewports = (payload_len - 1) / 6
 *
 * NOTE: There is NO "num_viewports" field — it's inferred from length.
 */
static void set_viewport(float width, float height) {
    float half_w = width / 2.0f;
    float half_h = height / 2.0f;

    /* Payload: start_slot + 6 floats = 7 words */
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7));
    emit(0);              /* start_slot */
    emit(f2u(half_w));    /* scale_x */
    emit(f2u(-half_h));   /* scale_y (negative = Y-flip for GL convention) */
    emit(f2u(0.5f));      /* scale_z */
    emit(f2u(half_w));    /* translate_x */
    emit(f2u(half_h));    /* translate_y */
    emit(f2u(0.5f));      /* translate_z */
}






/* Add this helper function at the top of virgl_pipeline.c */
static void print_hex(uint32_t val) {
    const char hex[] = "0123456789ABCDEF";
    char buf[11] = "0x";
    for (int i = 7; i >= 0; i--) {
        buf[2 + (7-i)] = hex[(val >> (i*4)) & 0xF];
    }
    buf[10] = '\0';
    serial_printf(buf);
}

static void print_dec(uint32_t val) {
    if (val == 0) {
        serial_printf("0");
        return;
    }
    char buf[12];
    int pos = 11;
    buf[pos--] = '\0';
    while (val > 0 && pos >= 0) {
        buf[pos--] = '0' + (val % 10);
        val /= 10;
    }
    serial_printf(&buf[pos + 1]);
}



/*
 * ================================================================
 *  PIPELINE SETUP — Orchestrates everything
 * ================================================================
 */
bool virgl_pipeline_setup(void) {
    ctx = virgl_get_ctx();
    if (!ctx || !ctx->initialized) {
        serial_printf("virgl_pipeline: virgl context not initialized!\n");
        return false;
    }

    serial_printf("virgl_pipeline: === BEGIN PIPELINE SETUP ===\n");

    bool ok;

    /*
     * Step 1: Create basic state objects (no resource references)
     * Submit in one batch — these never reference GPU resources.
     */
/* NEW - submit each object separately */
ctx->cmd_pos = 0;
ctx->blend_handle = create_blend_state(false);

// ADD THIS DEBUG:
serial_printf("virgl_pipeline: === BLEND STATE DUMP ===\n");
for (uint32_t i = 0; i < ctx->cmd_pos; i++) {
    serial_printf("  [");
    print_dec(i);
    serial_printf("] ");
    print_hex(ctx->cmd_buf[i]);
    serial_printf("\n");
}
serial_printf("virgl_pipeline: === END BLEND DUMP ===\n");

serial_printf("virgl_pipeline: submitting blend state\n");
ok = virgl_cmd_submit();
if (!ok) {
    serial_printf("virgl_pipeline: FAILED on blend state!\n");
    return false;
}

ctx->cmd_pos = 0;
ctx->rasterizer_handle = create_rasterizer_state();
serial_printf("virgl_pipeline: submitting rasterizer state\n");
ok = virgl_cmd_submit();
if (!ok) {
    serial_printf("virgl_pipeline: FAILED on rasterizer state!\n");
    return false;
}

ctx->cmd_pos = 0;
ctx->dsa_handle = create_dsa_state(true, true);
serial_printf("virgl_pipeline: submitting DSA state\n");
ok = virgl_cmd_submit();
if (!ok) {
    serial_printf("virgl_pipeline: FAILED on DSA state!\n");
    return false;
}

ctx->cmd_pos = 0;
ctx->ve_handle = create_vertex_elements();
serial_printf("virgl_pipeline: submitting vertex elements\n");
ok = virgl_cmd_submit();
if (!ok) {
    serial_printf("virgl_pipeline: FAILED on vertex elements!\n");
    return false;
}

/* Replace the debug dump code with: */
serial_printf("virgl_pipeline: === COMMAND BUFFER DUMP ===\n");
for (uint32_t i = 0; i < ctx->cmd_pos; i++) {
    uint32_t word = ctx->cmd_buf[i];
    uint8_t cmd = word & 0xFF;
    uint8_t obj_type = (word >> 8) & 0xFF;
    uint16_t length = (word >> 16) & 0xFFFF;
    
    serial_printf("  [");
    print_dec(i);
    serial_printf("] ");
    print_hex(word);
    
    /* Decode headers */
    if ((i == 0) || (cmd <= 30 && obj_type <= 10 && length <= 100)) {
        serial_printf(" <- HDR: cmd=");
        print_dec(cmd);
        serial_printf(" obj_type=");
        print_dec(obj_type);
        serial_printf(" len=");
        print_dec(length);
        
        if (cmd == 22) {
            serial_printf(" **WARNING: SET_POLYGON_STIPPLE**");
        }
    }
    serial_printf("\n");
}
serial_printf("virgl_pipeline: === END DUMP ===\n");

// ADD THIS HERE:
serial_printf("virgl_pipeline: === CHECKING FOR BUFFER OVERFLOW ===\n");
for (uint32_t i = 30; i < 36 && i < ctx->cmd_buf_size/4; i++) {
    serial_printf("  [");
    print_dec(i);
    serial_printf("] ");
    print_hex(ctx->cmd_buf[i]);
    serial_printf("\n");
}
serial_printf("virgl_pipeline: === END OVERFLOW CHECK ===\n");



    serial_printf("virgl_pipeline: submitting %u words (blend+rast+dsa+ve)\n", ctx->cmd_pos);
    ok = virgl_cmd_submit();
    if (!ok) {
        serial_printf("virgl_pipeline: FAILED on basic state objects!\n");
        return false;
    }
    serial_printf("virgl_pipeline: basic state objects OK\n");

    /*
     * Step 2: Create shaders (no resource references either)
     */
    ctx->cmd_pos = 0;

    /* Vertex shader: MVP transform */
    ctx->vs_handle = create_shader(PIPE_SHADER_VERTEX, vs_tgsi_text);

    serial_printf("virgl_pipeline: submitting %u words (vertex shader)\n", ctx->cmd_pos);
    ok = virgl_cmd_submit();
    if (!ok) {
        serial_printf("virgl_pipeline: FAILED on vertex shader!\n");
        return false;
    }
    serial_printf("virgl_pipeline: vertex shader OK\n");

    ctx->cmd_pos = 0;

    /* Fragment shader: pass-through color */
    ctx->fs_handle = create_shader(PIPE_SHADER_FRAGMENT, fs_tgsi_text);

    serial_printf("virgl_pipeline: submitting %u words (fragment shader)\n", ctx->cmd_pos);
    ok = virgl_cmd_submit();
    if (!ok) {
        serial_printf("virgl_pipeline: FAILED on fragment shader!\n");
        return false;
    }
    serial_printf("virgl_pipeline: fragment shader OK\n");

    /*
     * Step 3: Create surfaces (these reference GPU resources —
     *         requires resources to be CTX_ATTACHed)
     */
    ctx->cmd_pos = 0;

    /* Color surface */
    ctx->color_surface_handle = create_surface(ctx->fb_res_id,
                                                VIRGL_FORMAT_B8G8R8X8_UNORM);

    serial_printf("virgl_pipeline: submitting %u words (color surface, res=%u)\n",
                  ctx->cmd_pos, ctx->fb_res_id);
    ok = virgl_cmd_submit();
    if (!ok) {
        serial_printf("virgl_pipeline: FAILED on color surface! (res %u not attached to ctx?)\n",
                      ctx->fb_res_id);
        return false;
    }
    serial_printf("virgl_pipeline: color surface OK\n");

    ctx->cmd_pos = 0;

    /* Depth surface */
    ctx->depth_surface_handle = create_surface(ctx->depth_res_id,
                                                VIRGL_FORMAT_Z16_UNORM);

    serial_printf("virgl_pipeline: submitting %u words (depth surface, res=%u)\n",
                  ctx->cmd_pos, ctx->depth_res_id);
    ok = virgl_cmd_submit();
    if (!ok) {
        serial_printf("virgl_pipeline: FAILED on depth surface! (res %u not attached to ctx?)\n",
                      ctx->depth_res_id);
        return false;
    }
    serial_printf("virgl_pipeline: depth surface OK\n");

    serial_printf("virgl_pipeline: all objects created successfully\n");

    /*
     * Step 4: Bind all state objects
     */
    ctx->cmd_pos = 0;

    /* Bind blend state */
    bind_object(VIRGL_OBJECT_BLEND, ctx->blend_handle);

    /* Bind rasterizer state */
    bind_object(VIRGL_OBJECT_RASTERIZER, ctx->rasterizer_handle);

    /* Bind depth-stencil-alpha state */
    bind_object(VIRGL_OBJECT_DSA, ctx->dsa_handle);

    /* Bind vertex element layout */
    bind_object(VIRGL_OBJECT_VERTEX_ELEMENTS, ctx->ve_handle);

    /* Bind VS: shader_type=0 (vertex), then handle */
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_SHADER, 2));
    emit(PIPE_SHADER_VERTEX);   /* shader type */
    emit(ctx->vs_handle);       /* handle */

    /* Bind FS: shader_type=1 (fragment), then handle */
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_SHADER, 2));
    emit(PIPE_SHADER_FRAGMENT); /* shader type */
    emit(ctx->fs_handle);       /* handle */

    /* Set framebuffer state */
    set_framebuffer_state(ctx->color_surface_handle, ctx->depth_surface_handle);

    /* Set initial viewport to match framebuffer */
    set_viewport((float)ctx->fb_width, (float)ctx->fb_height);

    serial_printf("virgl_pipeline: submitting %u words of bind commands\n", ctx->cmd_pos);

    ok = virgl_cmd_submit();
    if (!ok) {
        serial_printf("virgl_pipeline: FAILED to submit bind commands!\n");
        return false;
    }

    serial_printf("virgl_pipeline: === PIPELINE SETUP COMPLETE ===\n");
    serial_printf("virgl_pipeline: handles: blend=%u rast=%u dsa=%u ve=%u vs=%u fs=%u\n",
                  ctx->blend_handle, ctx->rasterizer_handle, ctx->dsa_handle,
                  ctx->ve_handle, ctx->vs_handle, ctx->fs_handle);
    serial_printf("virgl_pipeline: surfaces: color=%u depth=%u\n",
                  ctx->color_surface_handle, ctx->depth_surface_handle);

    return true;
}

/*
 * ================================================================
 *  DYNAMIC STATE CHANGES
 * ================================================================
 *
 * These allow changing state at runtime (e.g., toggling depth test).
 * They create a new state object, submit it, then bind it.
 */

void virgl_pipeline_set_depth_test(bool enable) {
    ctx = virgl_get_ctx();
    if (!ctx || !ctx->initialized) return;

    ctx->cmd_pos = 0;

    /* Create a new DSA state */
    uint32_t new_dsa = create_dsa_state(enable, enable);

    /* Submit the creation */
    virgl_cmd_submit();

    /* Bind it */
    ctx->cmd_pos = 0;
    bind_object(VIRGL_OBJECT_DSA, new_dsa);
    virgl_cmd_submit();

    ctx->dsa_handle = new_dsa;
}