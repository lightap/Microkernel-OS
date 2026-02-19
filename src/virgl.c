#include "virgl.h"
#include "virtio.h"
#include "virtio_gpu.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "serial.h"
#include "vga.h"
#include "virgl.h"
#include "types.h"
#include "task.h"    



/* Minimal virgl protocol bits we need (avoid pulling virgl_protocol.h) */

/* Shader "offlen" field:
 *  - First packet: store FULL shader length in BYTES (low 31 bits), CONT bit clear
 *  - Continuation packet: store OFFSET in BYTES (low 31 bits), CONT bit set
 */
#define VIRGL_OBJ_SHADER_OFFSET_CONT        (1u << 31)
#define VIRGL_OBJ_SHADER_OFFSET_VAL(x_dwords) ((uint32_t)(x_dwords) & 0x7FFFFFFFu)
#define VIRGL_OBJ_SHADER_OFFSET_VAL_BYTES(x_bytes) ((uint32_t)(x_bytes) & 0x7FFFFFFFu)


static const uint8_t vs_bin[] __attribute__((aligned(4)))  = {  0x02, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x23, 0x30, 0x01, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x20, 0x20, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x30, 0x30, 0x2f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x20, 0x40, 0x0f, 0x01, 0x00, 0x00, 0x00, 0x00, 0x51, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x22, 0x10, 0xa0, 0x00, 0xc4, 0x00, 0x00, 0x00,
  0x07, 0x00, 0x40, 0x11, 0x22, 0x10, 0xa0, 0x00, 0x34, 0x00, 0x00, 0x00,
  0x02, 0x00, 0x00, 0x01, 0x22, 0x10, 0xa0, 0x00, 0xf3, 0x00, 0x00, 0x00,
  0x04, 0x00, 0x00, 0x39, 0x02, 0x50, 0x07, 0x00
};
static const uint8_t fs_bin[] __attribute__((aligned(4)))  = {   0x02, 0x0e, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x23, 0x50, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x30, 0x30, 0x2f, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x22, 0x10, 0xa0, 0x00, 0xf3, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x01,
  0x02, 0x50, 0x07, 0x00
 };


#define VS_BIN_LEN ((uint32_t)sizeof(vs_bin))
#define FS_BIN_LEN ((uint32_t)sizeof(fs_bin))


 static inline const uint32_t* as_u32(const uint8_t* p) { return (const uint32_t*)p; }
static inline uint32_t dwords(uint32_t bytes) { return bytes / 4; }




// Valid Vertex Shader Tokens (Position + Color)
// VERT / DCL IN[0],POSITION / DCL IN[1],GENERIC[0]
// DCL OUT[0],POSITION / DCL OUT[1],GENERIC[0]
// MOV OUT[0],IN[0] / MOV OUT[1],IN[1] / END
// Raw TGSI tokens for vertex shader (21 tokens)
static const uint32_t vs_tokens_binary[] = {
 // 0x00001802,
  0x00180202, 
    0x00000000,
    0x00013023,
    0x00000000,
    0x000F2020,
    0x00000000,
    0x002F3030,
    0x00000000,
    0x00000000,
    0x010F4020,
    0x00000000,
    0x00000051,
    0x00000000,
    0x3F800000,
    0x00000000,
    0x00000000,
    0x00a01022,
    0x000000C4,
    0x11400007,
    0x00A01022,
    0x00000034,
    0x01000002,
    0x00A01022,
    0x000000F3,
    0x39000004,
    0x00075002,
};



static const uint32_t fs_tokens_binary[] = {
   // 0x00000E02,
     0x000E0202,
    0x00000001,
    0x00005023,
    0x00000001,
    0x002F3030,
    0x00000000,
    0x00000001,
    0x00000051,
    0x3F800000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00A01022,
    0x000000F3,
    0x01000007,
    0x00075002,
};


/*
 * Virgl 3D Driver — GPU-accelerated rendering
 *
 * This replaces minigl's CPU rasterization with host GPU rendering.
 * Instead of computing every pixel on the CPU, we:
 *   1. Upload vertex data to a GPU resource (virgl buffer)
 *   2. Build a command buffer describing the draw calls
 *   3. Submit it to the host via VIRTIO_GPU_CMD_SUBMIT_3D
 *   4. The host's virglrenderer executes it on the real GPU
 *   5. Transfer the result to the display via SET_SCANOUT + FLUSH
 *
 * QEMU:
 *   qemu-system-i386 -device virtio-gpu-gl-pci -display gtk,gl=on -m 2G
 */

/* ===== External: the shared virtio device from virtio_gpu.c ===== */
static virtio_dev_t* virgl_dev = NULL;
static bool virgl_dev_initialized = false;

static virgl_ctx_t vctx;

/* Aligned command/response buffers for GPU commands */
/* Was 4096 — must be larger than biggest command + SUBMIT_3D header */
static uint8_t v3d_cmd_buf[131072]  __attribute__((aligned(4096)));
static uint8_t v3d_resp_buf[4096]   __attribute__((aligned(4096)));

static uint32_t* vctx_display_backing = NULL; // Add this global
static uint32_t* vctx_fb_backing = NULL; 



// ... rest of attach_backing code ...



/* ===== Low-level GPU command helper (same pattern as virtio_gpu.c) ===== */
static bool gpu3d_cmd(void* cmd, uint32_t cmd_len, void* resp, uint32_t resp_len) {
    memcpy(v3d_cmd_buf, cmd, cmd_len);
    memset(v3d_resp_buf, 0, resp_len);

    int head = virtio_send(virgl_dev, VIRTIO_GPU_QUEUE_CONTROL,
                           (uint32_t)v3d_cmd_buf, cmd_len,
                           (uint32_t)v3d_resp_buf, resp_len);
    if (head < 0) {
        //serial_printf("virgl: failed to submit gpu command\n");
        return false;
    }

    virtio_notify(virgl_dev, VIRTIO_GPU_QUEUE_CONTROL);
    virtio_wait(virgl_dev, VIRTIO_GPU_QUEUE_CONTROL);
    memcpy(resp, v3d_resp_buf, resp_len);

    virtio_gpu_ctrl_hdr_t* hdr = (virtio_gpu_ctrl_hdr_t*)resp;
    if (hdr->type >= VIRTIO_GPU_RESP_ERR_UNSPEC) {
        //serial_printf("virgl: gpu command error, type=%x\n", hdr->type);
        return false;
    }
    return true;
}

static bool gpu3d_cmd_ok(void* cmd, uint32_t cmd_len) {
    virtio_gpu_ctrl_hdr_t resp;
    return gpu3d_cmd(cmd, cmd_len, &resp, sizeof(resp));
}

/* ===== Allocate a resource ID ===== */
static uint32_t alloc_res_id(void) {
    return vctx.next_res_id++;
}

/* Append a uint32_t word to the command buffer */
//static inline void emit(uint32_t word) {
  //  if (vctx.cmd_pos < vctx.cmd_buf_size / 4) {
    //    vctx.cmd_buf[vctx.cmd_pos++] = word;
   // }
//}

static inline void emit(uint32_t word) {
    if (vctx.cmd_pos < (vctx.cmd_buf_size / 4)) {
        vctx.cmd_buf[vctx.cmd_pos++] = word;
    } else {
        //serial_printf("emit: overflow!\n");
    }
}


/* Pack a NUL-terminated string into the command buffer as uint32_t words (little-endian) */
static void emit_string(const char* str) {
    uint32_t len = strlen(str) + 1;  /* include NUL */
    const uint8_t* p = (const uint8_t*)str;
    uint32_t nwords = (len + 3) / 4;
    for (uint32_t w = 0; w < nwords; w++) {
        uint32_t word = 0;
        for (int b = 0; b < 4; b++) {
            uint32_t idx = w * 4 + b;
            if (idx < len)
                word |= ((uint32_t)p[idx]) << (b * 8);
        }
        emit(word);
    }
}

/* ===== Feature Negotiation ===== */
static bool virgl_negotiate_features(void) {
    volatile uint8_t* cfg = virgl_dev->common_cfg;
    if (!cfg) return false;

    /* Read features[0] */
    *(volatile uint32_t*)(cfg + VIRTIO_COMMON_DFSELECT) = 0;
    uint32_t features = *(volatile uint32_t*)(cfg + VIRTIO_COMMON_DF);

    //serial_printf("virgl: device features[0] = %x\n", features);

    if (!(features & (1 << VIRTIO_GPU_F_VIRGL))) {
        //serial_printf("virgl: VIRGL feature NOT supported!\n");
        //serial_printf("virgl: You need: -device virtio-gpu-gl-pci -display gtk,gl=on\n");
        return false;
    }

    //serial_printf("virgl: VIRGL feature available!\n");

    /* Accept VIRGL feature */
    *(volatile uint32_t*)(cfg + VIRTIO_COMMON_GFSELECT) = 0;
    *(volatile uint32_t*)(cfg + VIRTIO_COMMON_GF) = (1 << VIRTIO_GPU_F_VIRGL);

    /* Handle features[1] — accept VERSION_1 if offered */
    *(volatile uint32_t*)(cfg + VIRTIO_COMMON_DFSELECT) = 1;
    uint32_t features_hi = *(volatile uint32_t*)(cfg + VIRTIO_COMMON_DF);
    *(volatile uint32_t*)(cfg + VIRTIO_COMMON_GFSELECT) = 1;
    *(volatile uint32_t*)(cfg + VIRTIO_COMMON_GF) = features_hi & 1;

    /* Set FEATURES_OK */
    uint8_t s = *(volatile uint8_t*)(cfg + VIRTIO_COMMON_STATUS);
    *(volatile uint8_t*)(cfg + VIRTIO_COMMON_STATUS) = s | VIRTIO_STATUS_FEATURES_OK;

    s = *(volatile uint8_t*)(cfg + VIRTIO_COMMON_STATUS);
    if (!(s & VIRTIO_STATUS_FEATURES_OK)) {
        //serial_printf("virgl: device rejected VIRGL feature\n");
        return false;
    }

    return true;
}

/* ===== Create 3D Rendering Context ===== */
static bool virgl_create_context(void) {
    virtio_gpu_ctx_create_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    cmd.hdr.ctx_id = 1;       /* Context ID goes in header */
    cmd.nlen = 10;
    cmd.context_init = 0;     /* MUST be 0 for standard virgl (non-zero = capset flags) */
    memcpy(cmd.debug_name, "microgl3d", 10);

    //serial_printf("virgl: CTX_CREATE sizeof=%u (expect 96)\n", sizeof(cmd));

    if (!gpu3d_cmd_ok(&cmd, sizeof(cmd))) {
        //serial_printf("virgl: CTX_CREATE failed\n");
        return false;
    }

    vctx.ctx_id = 1;
    //serial_printf("virgl: created 3D context %u\n", vctx.ctx_id);
    return true;
}

/* ===== Create a 3D Resource ===== */
// src/virgl.c

bool virgl_create_resource_3d(uint32_t res_id, uint32_t target,
                              uint32_t fmt, uint32_t bind,
                              uint32_t width, uint32_t height, uint32_t depth)
{
    virtio_gpu_resource_create_3d_t cmd;
    memset(&cmd, 0, sizeof(cmd));

    cmd.hdr.type   = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    cmd.hdr.ctx_id = 0;   /* <--- MUST BE 0 (Global) */

    cmd.resource_id = res_id;
    cmd.target      = target;
    cmd.format      = fmt;
    cmd.bind        = bind;

    cmd.width       = width;
    cmd.height      = height;
    cmd.depth       = depth;

    cmd.array_size  = 1;
    cmd.last_level  = 0;
    cmd.nr_samples  = 0;  /* 0 = no multisampling (NOT 1!) */
    cmd.flags       = 0;
    cmd.padding     = 0;

    //serial_printf("virgl: CREATE_3D res=%u target=%u fmt=%u bind=%x %ux%u sizeof=%u (expect 72)\n",
             //     res_id, target, fmt, bind, width, height, sizeof(cmd));
//serial_printf("CREATE_3D DEBUG: res=%u format=%u\n", res_id, cmd.format);
    bool ok = gpu3d_cmd_ok(&cmd, sizeof(cmd));
    if (!ok) {
        //serial_printf("virgl: CREATE_3D FAILED for res=%u\n", res_id);
    }
    return ok;
}


/* ===== Attach Resource to Context ===== */
static bool virgl_ctx_attach(uint32_t res_id) {
    virtio_gpu_ctx_resource_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
    cmd.hdr.ctx_id = vctx.ctx_id;
    cmd.resource_id = res_id;

    //serial_printf("virgl: CTX_ATTACH res=%u ctx=%u sizeof=%u (expect 32)\n",
              //    res_id, vctx.ctx_id, sizeof(cmd));

    bool ok = gpu3d_cmd_ok(&cmd, sizeof(cmd));
    if (!ok) {
        //serial_printf("virgl: CTX_ATTACH FAILED for res=%u\n", res_id);
    } else {
        //serial_printf("virgl: CTX_ATTACH OK res=%u\n", res_id);
    }
    return ok;
}

/* ===== Attach Backing Store ===== */
static bool virgl_attach_backing(uint32_t res_id, uint32_t phys, uint32_t size) {
    struct {
        virtio_gpu_resource_attach_backing_t hdr;
        virtio_gpu_mem_entry_t entry;
    } __attribute__((packed)) cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd.hdr.hdr.ctx_id = 0;
    cmd.hdr.resource_id = res_id;
    cmd.hdr.nr_entries = 1;
    cmd.entry.addr = (uint64_t)phys;
    cmd.entry.length = size;

    return gpu3d_cmd_ok(&cmd, sizeof(cmd));
}
/* ============================================================
 * Debug dump (no %08x, no serial_putc)
 * ============================================================ */
/* ============================================================
 * Debug dump (no serial_putc needed)
 * ============================================================ */
#define VIRGL_DEBUG_DUMP 1
#define VIRGL_DEBUG_DUMP_MAX_WORDS 512

static void virgl_cmd_dump(uint32_t nwords)
{
#if VIRGL_DEBUG_DUMP
    if (nwords > VIRGL_DEBUG_DUMP_MAX_WORDS) nwords = VIRGL_DEBUG_DUMP_MAX_WORDS;

    //serial_printf("CMD_DUMP: %u words:\n", nwords);
    for (uint32_t i = 0; i < nwords; i++) {
        /* IMPORTANT:
         * Your //serial_printf("%x") already prints "0x........" in your logs.
         * So DO NOT add "0x" in the format string or you get "0x0x....".
         */
        //serial_printf("  [%u] %x\n", i, vctx.cmd_buf[i]);
    }
#else
    (void)nwords;
#endif
}

/* ============================================================
 * SUBMIT_3D helper (SIZE MUST BE BYTES)
 * ============================================================ */
static bool virgl_submit_cmd_buf(uint32_t *cmds, uint32_t size_bytes)
{
    if (!cmds) return false;
    if (size_bytes == 0) return true;

    /* SUBMIT_3D payload should be dword-aligned */
    if (size_bytes & 3u) {
        //serial_printf("virgl: submit_3d size not 4-byte aligned: %u\n", size_bytes);
        return false;
    }

    virtio_gpu_cmd_submit_3d_t *s = (virtio_gpu_cmd_submit_3d_t *)v3d_cmd_buf;
    const uint32_t total_bytes = (uint32_t)sizeof(*s) + size_bytes;

    /* Make sure the staging buffer can hold header + command stream */
    if (total_bytes > (uint32_t)sizeof(v3d_cmd_buf)) {
        //serial_printf("virgl: submit_3d overflow total=%u (hdr=%u + payload=%u) buf=%u\n",
                  //    total_bytes, (uint32_t)sizeof(*s), size_bytes, (uint32_t)sizeof(v3d_cmd_buf));
        return false;
    }

    memset(s, 0, sizeof(*s));
    s->hdr.type   = VIRTIO_GPU_CMD_SUBMIT_3D;
    s->hdr.ctx_id = vctx.ctx_id;
    s->size       = size_bytes; /* BYTES */

    uint8_t *dst = (uint8_t *)v3d_cmd_buf + sizeof(*s);

    /*
     * IMPORTANT: cmds might alias v3d_cmd_buf (depending on how you allocated vctx.cmd_buf).
     * memmove is safe for overlap; memcpy is not.
     */
    memmove(dst, cmds, size_bytes);

    memset(v3d_resp_buf, 0, sizeof(virtio_gpu_ctrl_hdr_t));

    int head = virtio_send(virgl_dev, VIRTIO_GPU_QUEUE_CONTROL,
                           (uint32_t)v3d_cmd_buf, total_bytes,
                           (uint32_t)v3d_resp_buf, sizeof(virtio_gpu_ctrl_hdr_t));
    if (head < 0) {
        //serial_printf("virgl: virtio_send submit_3d failed head=%d total_bytes=%u\n",
               //       head, total_bytes);
        return false;
    }

    virtio_notify(virgl_dev, VIRTIO_GPU_QUEUE_CONTROL);
    virtio_wait(virgl_dev, VIRTIO_GPU_QUEUE_CONTROL);

    virtio_gpu_ctrl_hdr_t *resp = (virtio_gpu_ctrl_hdr_t *)v3d_resp_buf;
    if (resp->type >= VIRTIO_GPU_RESP_ERR_UNSPEC) {
        //serial_printf("virgl: submit_3d failed resp.type=%x ctx=%u size_bytes=%u total=%u\n",
                  //    resp->type, vctx.ctx_id, size_bytes, total_bytes);
        return false;
    }

    return true;
}



bool virgl_cmd_submit(void)
{
    if (!vctx.initialized) return false;
    if (vctx.cmd_pos == 0) return true;

    /* cmd_pos is in dwords */
    uint32_t size_bytes = vctx.cmd_pos * 4;

    /* Safety: don't overflow the cmd buffer */
    if (size_bytes > vctx.cmd_buf_size) {
        //serial_printf("virgl_cmd_submit: size overflow (bytes=%u > buf=%u) cmd_pos=%u\n",
                  //    size_bytes, vctx.cmd_buf_size, vctx.cmd_pos);
        return false;
    }

    /* Optional but useful: dump exactly what we are about to submit */
 //   virgl_cmd_dump(vctx.cmd_pos);

    /*
     * IMPORTANT: SUBMIT_3D size is BYTES (not dwords).
     * virgl_submit_cmd_buf() must send ONLY 'size_bytes' of command data.
     */
    if (!virgl_submit_cmd_buf(vctx.cmd_buf, size_bytes)) {
        //serial_printf("virgl_cmd_submit: SUBMIT_3D failed (bytes=%u words=%u)\n",
                 //     size_bytes, vctx.cmd_pos);
        return false;
    }

    /* Reset command position after successful submit so next batch starts clean */
    vctx.cmd_pos = 0;

    return true;
}







/* ===== Float as uint32_t (for command buffer) ===== */
static inline uint32_t f2u(float f) {
    union { float f; uint32_t u; } u;
    u.f = f;
    return u.u;
}

/* ===== Public API Implementation ===== */

bool virgl_init(void) {
    if (vctx.initialized) return true;

    //serial_printf("virgl: initializing 3D support...\n");

    /* Verify struct sizes match QEMU expectations */
//serial_printf("virgl: struct sizes: ctrl_hdr=%u(24) ctx_create=%u(96) "
          //    "res_create_3d=%u(72) ctx_resource=%u(32) submit3d=%u(32)\n",
           //   sizeof(virtio_gpu_ctrl_hdr_t),
           //   sizeof(virtio_gpu_ctx_create_t),
           //   sizeof(virtio_gpu_resource_create_3d_t),
           //   sizeof(virtio_gpu_ctx_resource_t),
           //   sizeof(virtio_gpu_cmd_submit_3d_t));

//serial_printf("virgl: BUILD_ID=%x\n", VIRGL_BUILD_ID);
//serial_printf("virgl: HDR_TEST=%x\n",
          //    VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 6));



    memset(&vctx, 0, sizeof(vctx));
    vctx.next_res_id = 100;  /* Start high to avoid collision with 2D resources */

    /*
     * Use the SHARED device from virtio_gpu.c.  The 2D driver already
     * initialised the transport and negotiated the VIRGL feature bit
     * during virtio_gpu_init().  Re-initialising would reset the device
     * and destroy the 2D session.
     *
     * If the GUI is using BGA, virtio_gpu_init() may not have been
     * called yet — do it now.  It's idempotent (returns immediately
     * if already initialized).
     */
    if (!virtio_gpu_init()) {
        //serial_printf("virgl: failed to initialize virtio-gpu device\n");
        return false;
    }

    virtio_dev_t* shared = virtio_gpu_get_device();
    if (!shared) {
        //serial_printf("virgl: virtio-gpu device not initialized\n");
        return false;
    }

    if (!virtio_gpu_has_virgl()) {
        //serial_printf("virgl: VIRGL feature not available on this device\n");
        //serial_printf("virgl: Make sure QEMU uses: -device virtio-gpu-gl-pci -display gtk,gl=on\n");
        return false;
    }

    /* Use the shared device pointer directly (no copy — avoids queue state desync) */
    virgl_dev = shared;
    virgl_dev_initialized = true;

    //serial_printf("virgl: using shared virtio-gpu device (VIRGL negotiated)\n");

    /* Create 3D rendering context */
    if (!virgl_create_context()) {
        return false;
    }

    /* Allocate the command buffer (page-aligned for DMA) */
  vctx.cmd_buf = (uint32_t*)kmalloc(131072);  /* 128KB — was 64KB, sphere needs ~74KB */
if (!vctx.cmd_buf) {
    serial_printf("virgl: failed to allocate command buffer\n");
    return false;
}
vctx.cmd_buf_phys = (uint32_t)vctx.cmd_buf;
vctx.cmd_buf_size = 131072;
    vctx.cmd_pos = 0;

    vctx.initialized = true;
    //serial_printf("virgl: 3D initialization complete!\n");
    return true;
}

bool virgl_available(void) {
    if (vctx.initialized) return true;
    return virtio_gpu_has_virgl();
}

static void emit_bytes(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t*)data;
    uint32_t nwords = (len + 3) / 4;

    for (uint32_t w = 0; w < nwords; w++) {
        uint32_t word = 0;
        for (int b = 0; b < 4; b++) {
            uint32_t idx = w * 4 + b;
            if (idx < len)
                word |= ((uint32_t)p[idx]) << (b * 8);
        }
        emit(word);
    }
}


/* virgl shader IR type (this is NOT the stage) */
#define VIRGL_SHADER_IR_TGSI  0u   /* most common / safest */
static bool virgl_create_shader_text(uint32_t handle,
                                     uint32_t shader_type,
                                     const char *tgsi_dump_text,
                                     uint32_t num_tokens /* = binary_token_count */)
{
    uint32_t text_len  = (uint32_t)strlen(tgsi_dump_text) + 1; // include NUL
    uint32_t text_words = (text_len + 3) / 4;

    virgl_cmd_begin();

    // MUST be 6 + text_words (includes num_outputs dword)
   emit(VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER, 5 + text_words));
    emit(handle);
    emit(shader_type);

    // offlen is TOTAL shader text length in bytes (first chunk), CONT bit clear
    emit(VIRGL_OBJ_SHADER_OFFSET_VAL(text_len));
    emit(num_tokens);   // MUST be tgsi_num_tokens(tokens) / (bin_len/4)
    emit(0);            // num_outputs (streamout) = 0

    emit_bytes(tgsi_dump_text, text_len);

    return virgl_cmd_submit();
}

static bool virgl_create_shader(uint32_t handle, uint32_t shader_type,
                                const uint32_t *tokens, uint32_t num_tokens)
{
    if (num_tokens < 2) return false;

    virgl_cmd_begin();

    emit(VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER, 5 + num_tokens));
    emit(handle);
    emit(shader_type);
    emit(VIRGL_OBJ_SHADER_OFFSET_VAL_BYTES(num_tokens * 4u));  // byte length
    emit(num_tokens);
    emit(0); // num_so_outputs

    // Emit ALL tokens verbatim — token[0] is already the correct TGSI header
    for (uint32_t i = 0; i < num_tokens; i++)
        emit(tokens[i]);

    return virgl_cmd_submit();
}

void virgl_cmd_disable_depth(void)
{
    /* Bind a no-op DSA state: depth disabled, always pass */
    virgl_cmd_begin();
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_DSA, 5));
    emit(200);   /* one-shot temp handle */
    emit(0);     /* depth_enabled = 0 */
    emit(0);
    emit(0);
    emit(0);
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_DSA, 1));
    emit(200);
    virgl_cmd_submit();
}



#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

bool virgl_setup_pipeline_state(void)
{
    virgl_ctx_t *vctx = virgl_get_ctx();
    if (!vctx || !vctx->initialized) return false;

    //serial_printf("virgl_pipeline: === BEGIN PIPELINE SETUP ===\n");

    uint32_t handle;

    /* ---- Color surface ---- */
    handle = alloc_res_id();
    vctx->color_surface_handle = handle;
    virgl_cmd_begin();
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5));
    emit(handle);
    emit(vctx->fb_res_id);
    emit(VIRGL_FORMAT_B8G8R8A8_UNORM);  // 1
    emit(0);
    emit(0);
  //  emit(0); // last_layer
    //serial_printf("SURFACE COLOR submit (handle=%u res=%u)...\n", handle, vctx->fb_res_id);
    if (!virgl_cmd_submit()) { //serial_printf("SURFACE COLOR FAILED\n"); return false;
     }
    //serial_printf("SURFACE COLOR OK\n");


    /* ---- Depth surface ---- */
    handle = alloc_res_id();
    vctx->depth_surface_handle = handle;
    virgl_cmd_begin();
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5));
    emit(handle);
    emit(vctx->depth_res_id);
    emit(VIRGL_FORMAT_Z16_UNORM);
    emit(0);
    emit(0);
 //     emit(0); // last_layer
    //serial_printf("SURFACE DEPTH submit (handle=%u res=%u)...\n", handle, vctx->depth_res_id);
    if (!virgl_cmd_submit()) { //serial_printf("SURFACE DEPTH FAILED\n"); return false; 
    }
    //serial_printf("SURFACE DEPTH OK\n");

    /* ---- Blend ---- */
//    handle = alloc_res_id();
  //  vctx->blend_handle = 0;
    
   // //serial_printf("BLEND OK\n");

    handle = alloc_res_id();
    vctx->blend_handle = handle;
    virgl_cmd_begin();
emit(VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_BLEND, 11));
emit(handle);
emit(0);           // s0: logicop/dither flags
emit(0);           // s1: logicop func
for (int i = 0; i < 8; i++) {
    emit(0x78000000);  // colormask=0xF (RGBA) in bits[30:27], blend_enable=0
}
    //serial_printf("BLEND submit (handle=%u len=27)...\n", handle);
    if (!virgl_cmd_submit()) { //serial_printf("BLEND FAILED\n"); return false; 
    }
    //serial_printf("BLEND OK\n");


    /* ---- Rasterizer ---- */
    handle = alloc_res_id();
    vctx->rasterizer_handle = handle;
    virgl_cmd_begin();
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_RASTERIZER, 9));
    emit(handle);
    emit((1u << 1));
 //   emit((1u << 1) | (2u << 8));
    emit(f2u(1.0f));
    emit(0);
    emit(0);
    emit(f2u(1.0f));
    emit(0);
    emit(0);
    emit(0);
    //serial_printf("RAST submit (handle=%u len=9)...\n", handle);
    if (!virgl_cmd_submit()) { //serial_printf("RAST FAILED\n"); return false; 
    }
    //serial_printf("RAST OK\n");

    /* ---- DSA ---- */
    handle = alloc_res_id();
    vctx->dsa_handle = handle;
    virgl_cmd_begin();
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_DSA, 5));
    emit(handle);
    emit((1u << 0) | (1u << 1) | ((uint32_t)PIPE_FUNC_LESS << 2));
    emit(0);
    emit(0);
    emit(0);
    //serial_printf("DSA submit (handle=%u len=5)...\n", handle);
    if (!virgl_cmd_submit()) { //serial_printf("DSA FAILED\n"); return false; 
    }
    //serial_printf("DSA OK\n");

    /* ---- Vertex Elements ---- */
/* ---- Vertex Elements (2 elements: position + color) ---- */
handle = alloc_res_id();
vctx->ve_handle = handle;
virgl_cmd_begin();
emit(VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, 9));
emit(handle);
/* Element 0: position — offset 0, vec4 float */
emit(0);                            /* src_offset */
emit(0);                            /* instance_div */
emit(0);                            /* vb_index */
emit(PIPE_FORMAT_R32G32B32A32_FLOAT);
/* Element 1: color — offset 16 (after xyzw), vec4 float */
emit(16);                           /* src_offset = 4 floats * 4 bytes */
emit(0);                            /* instance_div */
emit(0);                            /* vb_index */
emit(PIPE_FORMAT_R32G32B32A32_FLOAT);
if (!virgl_cmd_submit()) { //serial_printf("VE FAILED\n"); return false;
 }
//serial_printf("VE OK\n");

    /* ---- Vertex Shader ---- */
static const char *vs_dump =
"VERT\n"
"DCL IN[0]\n"
"DCL IN[1]\n"
"DCL OUT[0], POSITION\n"
"DCL OUT[1], TEXCOORD[0]\n"
"  0: MOV OUT[0], IN[0]\n"
"  1: MOV OUT[1], IN[1]\n"
"  2: END\n";

static const char *fs_dump =
"FRAG\n"
"PROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n"
"DCL IN[0], TEXCOORD[0], LINEAR\n"
"DCL OUT[0], COLOR\n"
"  0: MOV OUT[0], IN[0]\n"
"  1: END\n";

handle = alloc_res_id();
vctx->vs_handle = handle;
if (!virgl_create_shader_text(handle, PIPE_SHADER_VERTEX,   vs_dump, 26)){
    //serial_printf("VS FAILED\n");
    return false;
}



handle = alloc_res_id();
vctx->fs_handle = handle;
if  (!virgl_create_shader_text(handle, PIPE_SHADER_FRAGMENT, fs_dump, 16)) {
    //serial_printf("FS FAILED\n");
    return false;
}


/*
uint32_t vs_tokens = dwords(VS_BIN_LEN);  // 104/4 = 26
uint32_t fs_tokens = dwords(FS_BIN_LEN);  //  64/4 = 16


handle = alloc_res_id();
vctx->vs_handle = handle;



//serial_printf("VS submit (handle=%u) [BINARY TGSI]...\n", handle);
if (!virgl_create_shader(handle, PIPE_SHADER_VERTEX,
                         as_u32(vs_bin), vs_tokens)){ return false;}
  

handle = alloc_res_id();
vctx->fs_handle = handle;

//serial_printf("FS submit (handle=%u) [BINARY TGSI]...\n", handle);
if (!virgl_create_shader(handle, PIPE_SHADER_FRAGMENT,
                         as_u32(fs_bin), fs_tokens)){ return false;}
   
*/
    virgl_cmd_begin() ;

    emit(VIRGL_CMD_HDR(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_BLEND, 1));
    emit(vctx->blend_handle);
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_RASTERIZER, 1));
    emit(vctx->rasterizer_handle);
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_DSA, 1));
    emit(vctx->dsa_handle);
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, 1));
    emit(vctx->ve_handle);

    emit(VIRGL_CMD_HDR(VIRGL_CCMD_BIND_SHADER, 0, 2));
    emit(vctx->vs_handle);
    emit(PIPE_SHADER_VERTEX);
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_BIND_SHADER, 0, 2));
    emit(vctx->fs_handle);
    emit(PIPE_SHADER_FRAGMENT);

    emit(VIRGL_CMD_HDR(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3));
    emit(1);
    emit(vctx->depth_surface_handle);
    emit(vctx->color_surface_handle);

emit(VIRGL_CMD_HDR(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7));
emit(0);                                     // viewport index
emit(f2u(vctx->fb_width  / 2.0f));          // scale.x = 160
emit(f2u(-(vctx->fb_height / 2.0f)));        // scale.y = -100  ← NEGATIVE, fixes Y flip
emit(f2u(0.5f));                             // scale.z = 0.5   ← maps NDC[-1,1]→[0,1]
emit(f2u(vctx->fb_width  / 2.0f));          // translate.x = 160
emit(f2u(vctx->fb_height / 2.0f));          // translate.y = 100
emit(f2u(0.5f));                             // translate.z = 0.5 ← centre of [0,1]

    emit(VIRGL_CMD_HDR(VIRGL_CCMD_SET_SCISSOR_STATE, 0, 3));
    emit(0);
    emit(0);
    emit(((uint32_t)vctx->fb_width) | ((uint32_t)vctx->fb_height << 16));

emit(VIRGL_CMD_HDR(VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, 3));
emit(32);              /* stride = 8 floats * 4 bytes = 32 */
emit(0);               /* offset */
emit(vctx->vbo_res_id);

    //serial_printf("BIND+STATE submit...\n");
    if (!virgl_cmd_submit()) { //serial_printf("BIND+STATE FAILED\n"); return false;
     }
    //serial_printf("BIND+STATE OK\n");

    //serial_printf("virgl_pipeline: === PIPELINE SETUP COMPLETE ===\n");
    //serial_printf("  surfaces: color=%u depth=%u\n", vctx->color_surface_handle, vctx->depth_surface_handle);
    //serial_printf("  state: blend=%u rast=%u dsa=%u ve=%u vs=%u fs=%u\n",
              //    vctx->blend_handle, vctx->rasterizer_handle, vctx->dsa_handle,
                //  vctx->ve_handle, vctx->vs_handle, vctx->fs_handle);
    return true;
}


bool virgl_setup_framebuffer(uint16_t width, uint16_t height)
{
    if (!vctx.initialized) return false;

    vctx.fb_width  = width;
    vctx.fb_height = height;

    const uint32_t fb_size = (uint32_t)width * (uint32_t)height * 4;

    /* ------------------------------------------------------------
     * 1) COLOR BUFFER (3D texture, render target)
     * ------------------------------------------------------------ */
    vctx.fb_res_id = alloc_res_id();
    //serial_printf("virgl: creating 3D color buffer resource %u\n", vctx.fb_res_id);

if (!virgl_create_resource_3d(vctx.fb_res_id,
                              PIPE_TEXTURE_2D,
                              VIRGL_FORMAT_B8G8R8A8_UNORM,  // 1
                              VIRGL_BIND_RENDER_TARGET,
                              width, height, 1))
    return false;

    void *fb_mem = kmalloc(fb_size + 4096);
    if (!fb_mem) return false;

    fb_mem = (void *)(((uint32_t)fb_mem + 4095) & ~4095);
    memset(fb_mem, 0, fb_size);

    vctx_fb_backing = (uint32_t *)fb_mem;

    uint32_t fb_phys = virt_to_phys(fb_mem);
    if (!virgl_attach_backing(vctx.fb_res_id, fb_phys, fb_size)) {
        //serial_printf("virgl: failed to attach backing to color buffer\n");
        return false;
    }
    if (!virgl_ctx_attach(vctx.fb_res_id)) {
        //serial_printf("virgl: failed to attach fb resource %u to context\n", vctx.fb_res_id);
        return false;
    }

    /* ------------------------------------------------------------
     * 2) DEPTH BUFFER (3D texture, depth/stencil)
     * ------------------------------------------------------------ */
    vctx.depth_res_id = alloc_res_id();
    //serial_printf("virgl: creating depth buffer resource %u\n", vctx.depth_res_id);

if (!virgl_create_resource_3d(vctx.depth_res_id,
                              PIPE_TEXTURE_2D,
                              VIRGL_FORMAT_Z16_UNORM,        // 142
                              VIRGL_BIND_DEPTH_STENCIL,
                              width, height, 1))
    return false;

    /* backing size for depth can be minimal; but keep yours for now */
    const uint32_t depth_size = (uint32_t)width * (uint32_t)height * 2; // Z16 = 2 bytes/px


    void *depth_mem = kmalloc(depth_size + 4096);
    if (!depth_mem) return false;

    depth_mem = (void *)(((uint32_t)depth_mem + 4095) & ~4095);
    memset(depth_mem, 0, depth_size);

    uint32_t depth_phys = virt_to_phys(depth_mem);
    if (!virgl_attach_backing(vctx.depth_res_id, depth_phys, depth_size)) {
        //serial_printf("virgl: failed to attach backing to depth buffer\n");
        return false;
    }
    if (!virgl_ctx_attach(vctx.depth_res_id)) {
        //serial_printf("virgl: failed to attach depth resource %u to context\n", vctx.depth_res_id);
        return false;
    }

    /* ------------------------------------------------------------
     * 3) VBO (PIPE_BUFFER)
     * ------------------------------------------------------------ */
    vctx.vbo_size   = 256 * 1024;
    vctx.vbo_res_id = alloc_res_id();
    //serial_printf("virgl: creating VBO resource %u\n", vctx.vbo_res_id);

    if (!virgl_create_resource_3d(vctx.vbo_res_id,
                                  PIPE_BUFFER,
                                  VIRGL_FORMAT_NONE,
                                  VIRGL_BIND_VERTEX_BUFFER,
                                  vctx.vbo_size, 1, 1))
    {
        //serial_printf("virgl: failed to create VBO\n");
        return false;
    }

    void *vbo_mem = kmalloc(vctx.vbo_size + 4096);
    if (!vbo_mem) return false;

    vbo_mem = (void *)(((uint32_t)vbo_mem + 4095) & ~4095);
    memset(vbo_mem, 0, vctx.vbo_size);

    uint32_t vbo_phys = virt_to_phys(vbo_mem);
    if (!virgl_attach_backing(vctx.vbo_res_id, vbo_phys, vctx.vbo_size)) {
        //serial_printf("virgl: failed to attach backing to VBO\n");
        return false;
    }
    if (!virgl_ctx_attach(vctx.vbo_res_id)) {
        //serial_printf("virgl: failed to attach vbo resource %u to context\n", vctx.vbo_res_id);
        return false;
    }

    /* ------------------------------------------------------------
     * 4) DISPLAY (3D texture used as scanout resource)
     * ------------------------------------------------------------ */
    vctx.display_res_id = alloc_res_id();
    //serial_printf("virgl: creating 3D display resource %u for scanout\n", vctx.display_res_id);

virtio_gpu_resource_create_2d_t cmd;
memset(&cmd, 0, sizeof(cmd));

cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
cmd.resource_id = vctx.display_res_id;
cmd.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
cmd.width  = width;
cmd.height = height;

if (!gpu3d_cmd_ok(&cmd, sizeof(cmd))) {
    //serial_printf("display CREATE_2D failed\n");
    return false;
}

    void *display_mem = kmalloc(fb_size + 4096);
    if (!display_mem) return false;

    display_mem = (void *)(((uint32_t)display_mem + 4095) & ~4095);
    memset(display_mem, 0, fb_size);

    vctx_display_backing = (uint32_t *)display_mem;

    uint32_t display_phys = virt_to_phys(display_mem);
    if (!virgl_attach_backing(vctx.display_res_id, display_phys, fb_size)) {
        //serial_printf("virgl: failed to attach backing to display resource\n");
        return false;
    }


// ADD THIS:
if (!virgl_ctx_attach(vctx.display_res_id)) {
    //serial_printf("virgl: failed to attach display resource %u to context\n", vctx.display_res_id);
    return false;
}



    /* Scanout */
    virtio_gpu_set_scanout_t scanout;
    memset(&scanout, 0, sizeof(scanout));
    scanout.hdr.type  = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout.hdr.ctx_id = 0;
    scanout.r.x = 0;
    scanout.r.y = 0;
    scanout.r.width  = width;
    scanout.r.height = height;
    scanout.scanout_id  = 0;
    scanout.resource_id = vctx.display_res_id;

    if (!gpu3d_cmd_ok(&scanout, sizeof(scanout))) {
        //serial_printf("virgl: SET_SCANOUT FAILED for resource %u!\n", vctx.display_res_id);
        return false;
    }
    //serial_printf("virgl: SET_SCANOUT succeeded for resource %u\n", vctx.display_res_id);

    //serial_printf("virgl: framebuffer %ux%u set up (color=%u, depth=%u, vbo=%u, display=%u)\n",
               //   width, height, vctx.fb_res_id, vctx.depth_res_id, vctx.vbo_res_id, vctx.display_res_id);

    return true;
}

virgl_ctx_t* virgl_get_ctx(void) {
    return &vctx;
}

/* ===== Command Buffer Building ===== */
/*
void virgl_cmd_begin(void) {
    vctx.cmd_pos = 0;
    for (uint32_t i = 0; i < vctx.cmd_buf_size / 4; i++) {
        vctx.cmd_buf[i] = 0;
    }
}*/

void virgl_cmd_begin(void)
{


    //serial_printf("VIRGL_BUILD_ID=%x HDR_TEST=%x\n",
           //   VIRGL_BUILD_ID,
             // VIRGL_CMD_HDR(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 6));
    vctx.cmd_pos = 0;
    //serial_printf("CMD_BEGIN: reset cmd_pos=0\n");
}



void virgl_cmd_set_viewport(uint32_t w, uint32_t h)
{
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7));
    emit(0);
    emit(f2u(w / 2.0f));
    emit(f2u(-(h / 2.0f)));   // negative
    emit(f2u(0.5f));
    emit(f2u(w / 2.0f));
    emit(f2u(h / 2.0f));
    emit(f2u(0.5f));
}

static inline void emit_u64(uint64_t v) {
    emit((uint32_t)(v & 0xFFFFFFFFu));
    emit((uint32_t)(v >> 32));
}

static inline uint64_t d2u(double d) {
    union { double d; uint64_t u; } x;
    x.d = d;
    return x.u;
}

void virgl_cmd_clear(uint32_t buffers, float r, float g, float b, float a,
                     double depth, uint32_t stencil)
{
    /* NOTE: Just emits into the command buffer — caller is responsible for
     * virgl_cmd_begin() before and virgl_cmd_submit() after.
     * This allows batching clear + draw commands into a single submit. */
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_CLEAR, 0, 8));
    emit(buffers);
    emit(f2u(r));
    emit(f2u(g));
    emit(f2u(b));
    emit(f2u(a));
    emit_u64(d2u(depth));
    emit(stencil);
}


/*
void virgl_cmd_set_viewport(float x, float y, float width, float height,
                            float near_val, float far_val) {
  
    float half_w = width / 2.0f;
    float half_h = height / 2.0f;
    float half_d = (far_val - near_val) / 2.0f;

    emit(VIRGL_CMD_HDR(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7));
    emit(0);  
    emit(f2u(half_w));              
    emit(f2u(-half_h));              
    emit(f2u(half_d));              
    emit(f2u(x + half_w));       
    emit(f2u(y + half_h));         
    emit(f2u(near_val + half_d));    
}*/



uint32_t virgl_upload_vertices(const float* data, uint32_t num_floats) {
    /*
     * RESOURCE_INLINE_WRITE: upload data directly into a GPU resource.
     * This is how we get vertex data to the GPU.
     *
     * Format: header + resource_id + level + usage + stride + layer_stride
     *       + x + y + z + w + h + d + data...
     * That's 11 header words + data words.
     */
    uint32_t size_bytes = num_floats * sizeof(float);
    uint32_t payload_words = 11 + (size_bytes + 3) / 4;  /* 11 header words + data */

    emit(VIRGL_CMD_HDR(VIRGL_CCMD_RESOURCE_INLINE_WRITE, 0, payload_words));
    emit(vctx.vbo_res_id);   /* resource */
    emit(0);                  /* level */
    emit(0);                  /* usage */
    emit(0);                  /* stride (not used for buffer) */
    emit(0);                  /* layer_stride */
    emit(0);                  /* x */
    emit(0);                  /* y */
    emit(0);                  /* z */
    emit(size_bytes);         /* w (width in bytes for buffers) */
    emit(1);                  /* h */
    emit(1);                  /* d */

    /* Copy vertex data as uint32_t words */
    const uint32_t* u = (const uint32_t*)data;
    for (uint32_t i = 0; i < num_floats; i++) {
        emit(u[i]);
    }

    return 0;  /* offset = 0 (we always write at start for simplicity) */
}

void virgl_cmd_set_vertex_buffer(uint32_t stride, uint32_t offset)
{
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, 3));
    emit(stride);
    emit(offset);
    emit(vctx.vbo_res_id);
}



void virgl_cmd_draw(uint32_t prim_mode, uint32_t start, uint32_t count) {
    /*
     * DRAW_VBO: header + 12 words
     */
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_DRAW_VBO, 0, 12));
    emit(start);         /* start */
    emit(count);         /* count */
    emit(prim_mode);     /* mode: PIPE_PRIM_TRIANGLES etc. */
    emit(0);             /* indexed = false */
    emit(1);             /* instance_count (MUST be >= 1, 0 = draw nothing!) */
    emit(0);             /* index_bias */
    emit(0);             /* start_instance */
    emit(0);             /* primitive_restart */
    emit(0);             /* restart_index */
    emit(0);             /* min_index */
    emit(0xFFFFFFFF);    /* max_index */
    emit(0);             /* cso (0 = use current) */
}

void virgl_cmd_set_constant_buffer(uint32_t shader_type,
                                   const float* data, uint32_t num_floats) {
    /*
     * SET_CONSTANT_BUFFER: header + shader_type + index + data...
     * Used to upload matrices (MVP) and uniforms.
     */
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_SET_CONSTANT_BUFFER, 0, 2 + num_floats));
    emit(shader_type);  /* PIPE_SHADER_VERTEX or PIPE_SHADER_FRAGMENT */
    emit(0);            /* index (constant buffer slot 0) */
    const uint32_t* u = (const uint32_t*)data;
    for (uint32_t i = 0; i < num_floats; i++) {
        emit(u[i]);
    }
}









void virgl_present(void)
{
    /* Ensure all 3D rendering is finished */
    (void)virgl_cmd_submit();

    /* 1) Copy color buffer -> display resource (host-side copy) */
    virgl_cmd_begin();
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_RESOURCE_COPY_REGION, 0, 13));
    emit(vctx.display_res_id); /* dst */
    emit(0); emit(0); emit(0); emit(0);
    emit(vctx.fb_res_id);      /* src */
    emit(0); emit(0); emit(0); emit(0);
    emit(vctx.fb_width);
    emit(vctx.fb_height);
    emit(1);

    if (!virgl_cmd_submit()) {
        //serial_printf("virgl_present: COPY_REGION failed\n");
        return;
    }

    /* 2) Flush scanout so QEMU updates the display */
    virtio_gpu_resource_flush_t flush;
    memset(&flush, 0, sizeof(flush));
    flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.hdr.ctx_id = 0;                 /* flush is 2D/control path */
    flush.resource_id = vctx.display_res_id;
    flush.r.x = 0;
    flush.r.y = 0;
    flush.r.width  = vctx.fb_width;
    flush.r.height = vctx.fb_height;

    if (!gpu3d_cmd_ok(&flush, sizeof(flush))) {
        //serial_printf("virgl_present: RESOURCE_FLUSH failed\n");
    }
}



void virgl_shutdown(void) {
    if (!vctx.initialized) return;

    /* Destroy context */
    virtio_gpu_ctrl_hdr_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    cmd.ctx_id = vctx.ctx_id;
    gpu3d_cmd_ok(&cmd, sizeof(cmd));

    if (vctx.cmd_buf) kfree(vctx.cmd_buf);
    memset(&vctx, 0, sizeof(vctx));

    //serial_printf("virgl: shutdown complete\n");
}


// Add this getter function:
uint32_t* virgl_get_display_backing(void) {
    return vctx_display_backing;
}

/*
 * ================================================================
 *  INTEGRATION GUIDE — How to merge this into your existing code
 * ================================================================
 *
 * STEP 1: Modify virtio.c to support feature negotiation
 * -------------------------------------------------------
 * Change virtio_init() to accept an optional feature mask:
 *
 *   bool virtio_init_with_features(virtio_dev_t* dev,
 *                                   uint16_t pci_device_id,
 *                                   uint32_t wanted_features);
 *
 * In the feature negotiation section (line ~237), instead of:
 *   mmio_write32(dev->common_cfg, VIRTIO_COMMON_GF, 0);
 * Do:
 *   uint32_t accepted = features & wanted_features;
 *   mmio_write32(dev->common_cfg, VIRTIO_COMMON_GF, accepted);
 *
 * STEP 2: Modify virtio_gpu.c to share the device
 * ------------------------------------------------
 * Add a getter at the bottom of virtio_gpu.c:
 *
 *   virtio_dev_t* virtio_gpu_get_device(void) {
 *       return &gpu_dev;
 *   }
 *
 * Then virgl.c uses the SAME device instead of creating a new one:
 *   virgl_dev = *virtio_gpu_get_device();
 *
 * STEP 3: Modify virtio_gpu_init() to try VIRGL first
 * ----------------------------------------------------
 *   bool virtio_gpu_init(void) {
 *       // Try with VIRGL feature
 *       if (virtio_init_with_features(&gpu_dev, VIRTIO_PCI_DEV_GPU,
 *                                      1 << VIRTIO_GPU_F_VIRGL)) {
 *           gpu_has_virgl = true;
 *       } else {
 *           // Fall back to 2D-only
 *           virtio_init(&gpu_dev, VIRTIO_PCI_DEV_GPU);
 *       }
 *       ...
 *   }
 *
 * STEP 4: In gui.c, choose rendering path
 * ----------------------------------------
 *   if (virgl_available()) {
 *       // GPU-accelerated path
 *       use_virgl_gl = true;
 *   } else {
 *       // Software minigl path (current)
 *       use_virgl_gl = false;
 *   }
 */