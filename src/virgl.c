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
static uint8_t v3d_cmd_buf[786432]  __attribute__((aligned(4096)));  /* fits full GPU scene batches */
static uint8_t v3d_resp_buf[4096]   __attribute__((aligned(4096)));

static uint32_t* vctx_display_backing = NULL; // Add this global
static uint32_t* vctx_fb_backing = NULL; 

/*
 * GPU submit lock. The GUI task (compiz composites) and an app task (GL
 * batches) both funnel through the shared staging buffers and virtqueue.
 * Preemption mid-submit would interleave ring/staging writes, so serialize.
 * Single CPU: cli() makes test-and-set atomic; holders never re-acquire.
 */
static volatile bool gpu_submit_lock = false;
static void gpu_lock_acquire(void) {
    for (;;) {
        cli();
        if (!gpu_submit_lock) {
            gpu_submit_lock = true;
            sti();
            return;
        }
        sti();
        task_yield();
    }
}
static void gpu_lock_release(void) {
    gpu_submit_lock = false;
}



// ... rest of attach_backing code ...



/* ===== Low-level GPU command helper (same pattern as virtio_gpu.c) ===== */
static bool gpu3d_cmd(void* cmd, uint32_t cmd_len, void* resp, uint32_t resp_len) {
    gpu_lock_acquire();
    memcpy(v3d_cmd_buf, cmd, cmd_len);
    memset(v3d_resp_buf, 0, resp_len);

    int head = virtio_send(virgl_dev, VIRTIO_GPU_QUEUE_CONTROL,
                           (uint32_t)v3d_cmd_buf, cmd_len,
                           (uint32_t)v3d_resp_buf, resp_len);
    if (head < 0) {
        //serial_printf("virgl: failed to submit gpu command\n");
        gpu_lock_release();
        return false;
    }

    virtio_notify(virgl_dev, VIRTIO_GPU_QUEUE_CONTROL);
    virtio_wait(virgl_dev, VIRTIO_GPU_QUEUE_CONTROL);
    memcpy(resp, v3d_resp_buf, resp_len);
    gpu_lock_release();

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

/* ================================================================
 * Windowed GL apps (per-app virgl sub-contexts)
 *
 * With the compiz compositor active, a GL app no longer takes over the
 * scanout. Instead it gets its own virgl SUB-CONTEXT (isolated GL state
 * inside the one virtio-gpu context) and renders into its own texture,
 * which is read back into guest RAM and composited by the GUI like any
 * other window content.
 * ================================================================ */
static bool     app_windowed   = false;  /* app renders to texture, not scanout */
static uint32_t app_sub_ctx    = 0;      /* sub-context id used by the app */
static uint16_t app_fb_w = 0, app_fb_h = 0;
static bool     setup_skip_display = false; /* setup_framebuffer: skip display/scanout */

static inline void emit(uint32_t word) {
    /*
     * Auto-tag batches with the app's sub-context: whenever a new batch
     * starts (cmd_pos == 0), prepend SET_SUB_CTX(app_sub_ctx) so all app
     * GL state/draws land in the app's sub-context no matter which code
     * path started the batch. The compiz compositor never uses emit()
     * (it builds its batches in local buffers tagged SET_SUB_CTX 0).
     */
    if (app_windowed && vctx.cmd_pos == 0 && vctx.cmd_buf_size >= 8) {
        vctx.cmd_buf[0] = VIRGL_CMD_HDR(VIRGL_CCMD_SET_SUB_CTX, 0, 1);
        vctx.cmd_buf[1] = app_sub_ctx;
        vctx.cmd_pos = 2;
    }
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

    gpu_lock_acquire();

    virtio_gpu_cmd_submit_3d_t *s = (virtio_gpu_cmd_submit_3d_t *)v3d_cmd_buf;
    const uint32_t total_bytes = (uint32_t)sizeof(*s) + size_bytes;

    /* Make sure the staging buffer can hold header + command stream */
    if (total_bytes > (uint32_t)sizeof(v3d_cmd_buf)) {
        //serial_printf("virgl: submit_3d overflow total=%u (hdr=%u + payload=%u) buf=%u\n",
                  //    total_bytes, (uint32_t)sizeof(*s), size_bytes, (uint32_t)sizeof(v3d_cmd_buf));
        gpu_lock_release();
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
        gpu_lock_release();
        return false;
    }

    virtio_notify(virgl_dev, VIRTIO_GPU_QUEUE_CONTROL);
    virtio_wait(virgl_dev, VIRTIO_GPU_QUEUE_CONTROL);

    virtio_gpu_ctrl_hdr_t *resp = (virtio_gpu_ctrl_hdr_t *)v3d_resp_buf;
    if (resp->type >= VIRTIO_GPU_RESP_ERR_UNSPEC) {
        //serial_printf("virgl: submit_3d failed resp.type=%x ctx=%u size_bytes=%u total=%u\n",
                  //    resp->type, vctx.ctx_id, size_bytes, total_bytes);
        gpu_lock_release();
        return false;
    }

    gpu_lock_release();
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
    vctx.vbo_size   = 1024 * 1024;  /* holds the GPU desktop scene (512KB verts) */
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
     * Skipped for windowed apps: the compiz compositor already owns
     * the display resource and scanout — the app renders only to its
     * own fb texture, which is read back and composited as a window.
     * ------------------------------------------------------------ */
    if (setup_skip_display) {
        //serial_printf("virgl: windowed setup — skipping display/scanout\n");
        return true;
    }

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









/* Perf instrumentation: cycles spent inside submit vs flush, read and
 * reset by the FPS reporter in syscall.c */
uint64_t virgl_stat_submit_tsc = 0;
uint64_t virgl_stat_flush_tsc  = 0;

void virgl_present(void)
{
    uint64_t t0 = rdtsc();

    if (app_windowed) {
        /*
         * Windowed present: submit the frame batch (auto-tagged with the
         * app's sub-context by emit()), then read the rendered color
         * buffer back into its guest backing. The GUI composites those
         * pixels into the app's desktop window like any other content —
         * Z-order, dragging, and the cursor stay correct for free.
         */
        if (!virgl_cmd_submit()) return;

        uint64_t t1 = rdtsc();
        virgl_stat_submit_tsc += t1 - t0;

        virtio_gpu_transfer_host_3d_t xfer;
        memset(&xfer, 0, sizeof(xfer));
        xfer.hdr.type   = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
        xfer.hdr.ctx_id = vctx.ctx_id;
        xfer.box.x = 0;
        xfer.box.y = 0;
        xfer.box.z = 0;
        xfer.box.w = app_fb_w;
        xfer.box.h = app_fb_h;
        xfer.box.d = 1;
        xfer.offset       = 0;
        xfer.resource_id  = vctx.fb_res_id;
        xfer.level        = 0;
        xfer.stride       = (uint32_t)app_fb_w * 4;
        xfer.layer_stride = 0;
        if (!gpu3d_cmd_ok(&xfer, sizeof(xfer))) {
            serial_printf("virgl: windowed readback failed\n");
        }

        virgl_stat_flush_tsc += rdtsc() - t1;
        return;
    }
    /*
     * Emit the color-buffer -> display-resource copy INTO the pending
     * frame batch (clear + vertex upload + draw are still unsubmitted),
     * then submit everything in ONE SUBMIT_3D round-trip. Previously
     * this was two round-trips: submit(frame), then submit(copy).
     * Each round-trip that misses the spin window costs a 10ms tick,
     * so halving them matters.
     */
    emit(VIRGL_CMD_HDR(VIRGL_CCMD_RESOURCE_COPY_REGION, 0, 13));
    emit(vctx.display_res_id); /* dst */
    emit(0); emit(0); emit(0); emit(0);
    emit(vctx.fb_res_id);      /* src */
    emit(0); emit(0); emit(0); emit(0);
    emit(vctx.fb_width);
    emit(vctx.fb_height);
    emit(1);

    if (!virgl_cmd_submit()) {
        //serial_printf("virgl_present: submit failed\n");
        return;
    }

    uint64_t t1 = rdtsc();
    virgl_stat_submit_tsc += t1 - t0;

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

    virgl_stat_flush_tsc += rdtsc() - t1;
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

/* True while a virgl 3D context owns scanout 0 (i.e. a GL app is on screen).
 * The 2D GUI compositor uses this to stop repainting the invisible desktop. */
bool virgl_scanout_active(void) {
    return vctx.initialized;
}

/* ================================================================
 *  "compiz" mode — GPU-composited desktop
 *
 *  Pipeline per frame:
 *    1. GUI renders the desktop into the guest backing buffer (CPU,
 *       pixel-identical to the classic path).
 *    2. TRANSFER_TO_HOST_3D DMAs only the dirty row band into a virgl
 *       GPU texture (compiz_tex).
 *    3. A batched RESOURCE_COPY_REGION composites the band from the
 *       texture to the display resource — executed by virglrenderer
 *       as a GL operation on the host GPU.
 *    4. RESOURCE_FLUSH updates the visible scanout.
 *
 *  Everything below reuses command paths already proven by hello.elf
 *  (resource_create_3d, attach_backing, ctx_attach, copy_region, flush).
 * ================================================================ */

static uint32_t compiz_tex_id = 0;
static uint16_t compiz_w = 0, compiz_h = 0;

/* Orientation: QEMU with gl=on typically displays GL-written scanout
 * resources bottom-up, so the DEFAULT is flip=true: chrome is drawn
 * GL-upright and CPU content is uploaded row-mirrored, both landing
 * upright on a flipped scanout. If a host/driver combo shows it the
 * other way, the backtick (`) key toggles this live. */
static bool compiz_flip = true;

/* Texturing (wobbly windows) is opt-in: every automatic attempt so far
 * has context-errored some hosts to a black screen. Plain `compiz` runs
 * the proven pipeline only; `compiz wobble` arms this. */
static bool compiz_want_texturing = false;
void virgl_compiz_enable_texturing(bool on) { compiz_want_texturing = on; }

/* Dedicated staging buffer backing the desktop texture (row-mirroring
 * happens on copy-in, so it can't be the GUI backbuffer itself). */
static uint32_t* compiz_stage = NULL;

/* Saved at compiz init: the compositor's own resources, so a windowed
 * app re-running virgl_setup_framebuffer can't steal them from us. */
static uint32_t compiz_fb_res   = 0;
static uint32_t compiz_vbo_res  = 0;
static uint32_t compiz_vbo_size = 0;

bool virgl_compiz_active(void) {
    return compiz_tex_id != 0;
}

void virgl_compiz_set_flip(bool f) { compiz_flip = f; }
bool virgl_compiz_get_flip(void)   { return compiz_flip; }

bool virgl_compiz_init(uint16_t w, uint16_t h, void* backing) {
    if (!backing) return false;

    if (!virgl_init()) {
        serial_printf("compiz: virgl_init failed\n");
        return false;
    }
    /* Creates fb/depth/vbo/display resources at desktop resolution and
     * issues SET_SCANOUT(display_res) — same proven path hello.elf uses. */
    if (!virgl_setup_framebuffer(w, h)) {
        serial_printf("compiz: framebuffer setup failed\n");
        return false;
    }

    /* Desktop texture: a host GPU texture whose backing store is the GUI's
     * render buffer, so dirty bands can be DMA'd straight from it. */
    compiz_tex_id = alloc_res_id();
    if (!virgl_create_resource_3d(compiz_tex_id,
                                  PIPE_TEXTURE_2D,
                                  VIRGL_FORMAT_B8G8R8X8_UNORM,
                                  VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_RENDER_TARGET,
                                  w, h, 1)) {
        serial_printf("compiz: desktop texture create failed\n");
        compiz_tex_id = 0;
        return false;
    }

    (void)backing;  /* content now arrives via the internal staging buffer */
    compiz_stage = (uint32_t*)kmalloc((uint32_t)w * h * 4 + PAGE_SIZE);
    if (!compiz_stage) {
        serial_printf("compiz: staging alloc failed\n");
        compiz_tex_id = 0;
        return false;
    }
    compiz_stage = (uint32_t*)(((uint32_t)compiz_stage + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    memset(compiz_stage, 0, (uint32_t)w * h * 4);

    uint32_t backing_phys = virt_to_phys(compiz_stage);
    if (!virgl_attach_backing(compiz_tex_id, backing_phys, (uint32_t)w * h * 4)) {
        serial_printf("compiz: attach backing failed\n");
        compiz_tex_id = 0;
        return false;
    }
    if (!virgl_ctx_attach(compiz_tex_id)) {
        serial_printf("compiz: ctx attach failed\n");
        compiz_tex_id = 0;
        return false;
    }

    compiz_w = w;
    compiz_h = h;

    /* The compositor renders the desktop chrome as GPU geometry, so it
     * needs full pipe state (shaders, DSA, vertex elements, framebuffer
     * binding, viewport) in sub-context 0. This runs before any windowed
     * app exists, so all batches land in sub-ctx 0 untagged. */
    if (!virgl_setup_pipeline_state()) {
        serial_printf("compiz: pipeline setup failed — GPU scene disabled\n");
        compiz_fb_res = 0;
    } else {
        virgl_cmd_set_viewport(w, h);
        virgl_cmd_submit();
        /* Capture our resource ids NOW: a windowed app re-running
         * virgl_setup_framebuffer later overwrites the vctx fields. */
        compiz_fb_res   = vctx.fb_res_id;
        compiz_vbo_res  = vctx.vbo_res_id;
        compiz_vbo_size = vctx.vbo_size;
        serial_printf("compiz: GPU scene pipeline ready (fb=%u vbo=%u)\n",
                      compiz_fb_res, compiz_vbo_res);
        /* Texture sampling for wobbly windows (desktop tex as SAMP[0]).
         * OPT-IN via `compiz wobble` — a rejected packet can poison the
         * whole virgl context (black screen), so plain compiz never
         * risks it. */
        if (compiz_want_texturing) {
            extern bool virgl_pipeline_setup_texturing(uint32_t tex_res_id);
            if (!virgl_pipeline_setup_texturing(compiz_tex_id))
                serial_printf("compiz: texturing unavailable — wobble disabled\n");
        } else {
            serial_printf("compiz: wobble not armed (use 'compiz wobble')\n");
        }
    }

    serial_printf("compiz: GPU compositor up — %ux%u, tex=%u display=%u\n",
                  w, h, compiz_tex_id, vctx.display_res_id);
    return true;
}

void virgl_compiz_present(uint32_t y0, uint32_t band_h) {
    if (!compiz_tex_id || !vctx.initialized) return;
    if (y0 >= compiz_h) return;
    if (y0 + band_h > compiz_h) band_h = compiz_h - y0;
    if (band_h == 0) return;

    const uint32_t stride = (uint32_t)compiz_w * 4;

    /* 1) DMA the dirty band: guest backing -> host texture */
    virtio_gpu_transfer_host_3d_t xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.hdr.type   = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    xfer.hdr.ctx_id = vctx.ctx_id;
    xfer.box.x = 0;
    xfer.box.y = y0;
    xfer.box.z = 0;
    xfer.box.w = compiz_w;
    xfer.box.h = band_h;
    xfer.box.d = 1;
    xfer.offset       = (uint64_t)y0 * stride;
    xfer.resource_id  = compiz_tex_id;
    xfer.level        = 0;
    xfer.stride       = stride;
    xfer.layer_stride = 0;
    if (!gpu3d_cmd_ok(&xfer, sizeof(xfer))) {
        serial_printf("compiz: transfer_to_host_3d failed (y=%u h=%u)\n", y0, band_h);
        return;
    }

    /* 2) GPU-side composite: texture band -> display resource.
     * Built in a LOCAL buffer, not the shared vctx.cmd_buf: a GL app may
     * have a half-built batch in there across syscalls. Tagged with
     * SET_SUB_CTX 0 so the copy always runs in the compositor's
     * sub-context regardless of what the app selected last. */
    {
        uint32_t cbuf[16];
        int n = 0;
        cbuf[n++] = VIRGL_CMD_HDR(VIRGL_CCMD_SET_SUB_CTX, 0, 1);
        cbuf[n++] = 0;                     /* sub-context 0 = compositor */
        cbuf[n++] = VIRGL_CMD_HDR(VIRGL_CCMD_RESOURCE_COPY_REGION, 0, 13);
        cbuf[n++] = vctx.display_res_id;   /* dst handle */
        cbuf[n++] = 0;                     /* dst level */
        cbuf[n++] = 0;                     /* dst x */
        cbuf[n++] = y0;                    /* dst y */
        cbuf[n++] = 0;                     /* dst z */
        cbuf[n++] = compiz_tex_id;         /* src handle */
        cbuf[n++] = 0;                     /* src level */
        cbuf[n++] = 0;                     /* src x */
        cbuf[n++] = y0;                    /* src y */
        cbuf[n++] = 0;                     /* src z */
        cbuf[n++] = compiz_w;              /* width  */
        cbuf[n++] = band_h;                /* height */
        cbuf[n++] = 1;                     /* depth  */
        if (!virgl_submit_cmd_buf(cbuf, (uint32_t)n * 4)) {
            serial_printf("compiz: composite submit failed\n");
            return;
        }
    }

    /* 3) Flush the band to the visible scanout */
    virtio_gpu_resource_flush_t flush;
    memset(&flush, 0, sizeof(flush));
    flush.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.resource_id = vctx.display_res_id;
    flush.r.x = 0;
    flush.r.y = y0;
    flush.r.width  = compiz_w;
    flush.r.height = band_h;
    gpu3d_cmd_ok(&flush, sizeof(flush));
}

void virgl_compiz_shutdown(void) {
    if (compiz_tex_id) {
        /* Detach + unref the desktop texture so the id can be reused */
        virtio_gpu_resource_detach_backing_t detach;
        memset(&detach, 0, sizeof(detach));
        detach.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
        detach.resource_id = compiz_tex_id;
        gpu3d_cmd_ok(&detach, sizeof(detach));

        virtio_gpu_resource_unref_t unref;
        memset(&unref, 0, sizeof(unref));
        unref.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_UNREF;
        unref.resource_id = compiz_tex_id;
        gpu3d_cmd_ok(&unref, sizeof(unref));

        compiz_tex_id = 0;
    }
    compiz_w = compiz_h = 0;
    virgl_shutdown();
}

/* ================================================================
 *  Windowed GL app lifecycle (sub-context per app)
 * ================================================================ */

static bool virgl_sub_ctx_cmd(uint32_t ccmd, uint32_t id) {
    uint32_t buf[2];
    buf[0] = VIRGL_CMD_HDR(ccmd, 0, 1);
    buf[1] = id;
    return virgl_submit_cmd_buf(buf, sizeof(buf));
}

bool virgl_app_windowed_active(void) {
    return app_windowed;
}

/* Begin windowed app setup: create + select the app's sub-context and
 * switch resource setup into "no display/scanout" mode. The caller then
 * runs virgl_setup_framebuffer + virgl_setup_pipeline_state as usual —
 * emit() auto-prefixes every batch with SET_SUB_CTX(app), so all pipe
 * state lands in the app's sub-context, isolated from the compositor. */
bool virgl_app_windowed_begin(uint16_t w, uint16_t h) {
    if (!vctx.initialized || !virgl_compiz_active()) return false;
    if (app_windowed) return false;   /* one windowed app at a time (v1) */

    if (!virgl_sub_ctx_cmd(VIRGL_CCMD_CREATE_SUB_CTX, 1)) {
        serial_printf("virgl: CREATE_SUB_CTX failed\n");
        return false;
    }
    app_sub_ctx  = 1;
    app_windowed = true;
    setup_skip_display = true;
    app_fb_w = w;
    app_fb_h = h;
    serial_printf("virgl: windowed app sub-context %u created (%ux%u)\n",
                  app_sub_ctx, w, h);
    return true;
}

/* Called after framebuffer+pipeline setup succeeded (or to abort). */
void virgl_app_windowed_commit(bool ok) {
    setup_skip_display = false;
    if (!ok) {
        virgl_sub_ctx_cmd(VIRGL_CCMD_DESTROY_SUB_CTX, app_sub_ctx);
        app_windowed = false;
        app_sub_ctx  = 0;
        app_fb_w = app_fb_h = 0;
    }
}

/* GUI accessor: the app's rendered pixels (guest backing of its color
 * buffer, filled by the readback in virgl_present). */
uint32_t* virgl_app_backing(uint16_t* w, uint16_t* h) {
    if (!app_windowed || !vctx_fb_backing) return NULL;
    if (w) *w = app_fb_w;
    if (h) *h = app_fb_h;
    return vctx_fb_backing;
}

void virgl_app_windowed_end(void) {
    if (!app_windowed) return;
    /* Drop any half-built batch the (possibly killed) app left behind,
     * so the next batch starts clean at cmd_pos 0. */
    vctx.cmd_pos = 0;
    virgl_sub_ctx_cmd(VIRGL_CCMD_DESTROY_SUB_CTX, app_sub_ctx);
    app_windowed = false;
    app_sub_ctx  = 0;
    app_fb_w = app_fb_h = 0;
    serial_printf("virgl: windowed app sub-context destroyed\n");
    /* App fb/depth/vbo resources are leaked host-side until virgl teardown
     * — acceptable for v1 (ids keep incrementing, no collisions). */
}

/* Exposed so the GUI can quiesce the GPU pipe before killing a GL app:
 * acquiring the lock waits for any in-flight submit by the app to finish,
 * ensuring the app never dies while holding the lock (which would
 * deadlock the compositor forever). */
void virgl_lock_gpu(void)   { gpu_lock_acquire(); }
void virgl_unlock_gpu(void) { gpu_lock_release(); }

/* ================================================================
 *  GPU SCENE RENDERER — the desktop drawn BY the GPU
 *
 *  The desktop chrome (background gradient, window frames, titlebars,
 *  title text, taskbar, start button, cursor) is submitted as real
 *  triangle geometry each frame and rendered by the host GPU through
 *  virglrenderer. Text-heavy/dynamic content (window client areas,
 *  taskbar strip, start menu) stays CPU-rendered and is composited on
 *  top from the desktop texture as opaque rectangles.
 *
 *  The whole scene batch lives in its own static buffer — never the
 *  shared vctx.cmd_buf, which may hold a GL app's half-built batch —
 *  and is tagged SET_SUB_CTX 0 so it uses the compositor's pipe state
 *  (shaders/framebuffer/DSA bound at compiz init), isolated from any
 *  windowed app's sub-context state.
 *
 *  Depth func is LESS with clear-depth 1.0, so painter's order is
 *  enforced by strictly decreasing z per primitive.
 * ================================================================ */

#define SCENE_MAX_VERTS 16384                 /* x 32 bytes = 512 KB */
static float    scene_verts[SCENE_MAX_VERTS * 8];
static uint32_t scene_nverts = 0;
#define SCENE_MAX_TEX_VERTS 4096              /* x 32B = 128 KB */
static float    scene_tex_verts[SCENE_MAX_TEX_VERTS * 8];
static uint32_t scene_tex_nverts = 0;
static float    scene_z = 0.999f;
static uint32_t scene_cmd[176000];            /* ~688 KB batch build buffer */

void virgl_scene_begin(void) {
    scene_nverts = 0;
    scene_tex_nverts = 0;
    scene_z = 0.999f;
}

static void scene_vtx(float px, float py, uint32_t rgb) {
    if (scene_nverts >= SCENE_MAX_VERTS) return;
    float* v = &scene_verts[scene_nverts * 8];
    v[0] = -1.0f + 2.0f * px / (float)compiz_w;
    /* Orientation-aware. The pipeline's negative-viewport hack already
     * flips GL output top-down in the texture, so on a flipped scanout
     * (flip=true) chrome must be drawn with screen-top -> NDC -1 to land
     * row-mirrored in the texture like the CPU content does. (Confirmed
     * empirically: the +1 mapping put the GPU taskbar at the screen top
     * and titlebars at window bottoms.) Toggled live with `. */
    v[1] = compiz_flip ? (-1.0f + 2.0f * py / (float)compiz_h)
                       : ( 1.0f - 2.0f * py / (float)compiz_h);
    v[2] = scene_z;
    v[3] = 1.0f;
    v[4] = (float)((rgb >> 16) & 0xFF) / 255.0f;
    v[5] = (float)((rgb >>  8) & 0xFF) / 255.0f;
    v[6] = (float)( rgb        & 0xFF) / 255.0f;
    v[7] = 1.0f;
    scene_nverts++;
}

static void scene_step_z(void) {
    scene_z -= 0.00006f;
    if (scene_z < 0.001f) scene_z = 0.001f;
}

/* Quad with per-corner colors (GPU interpolates the gradient) */
void virgl_scene_quad4(int x, int y, int w, int h,
                       uint32_t c00, uint32_t c10, uint32_t c11, uint32_t c01) {
    if (w <= 0 || h <= 0) return;
    scene_vtx((float)x,     (float)y,     c00);
    scene_vtx((float)(x+w), (float)y,     c10);
    scene_vtx((float)(x+w), (float)(y+h), c11);
    scene_vtx((float)x,     (float)y,     c00);
    scene_vtx((float)(x+w), (float)(y+h), c11);
    scene_vtx((float)x,     (float)(y+h), c01);
    scene_step_z();
}

void virgl_scene_quad(int x, int y, int w, int h, uint32_t c) {
    virgl_scene_quad4(x, y, w, h, c, c, c, c);
}

void virgl_scene_tri(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c) {
    scene_vtx((float)x0, (float)y0, c);
    scene_vtx((float)x1, (float)y1, c);
    scene_vtx((float)x2, (float)y2, c);
    scene_step_z();
}

uint32_t virgl_scene_free_verts(void) {
    return SCENE_MAX_VERTS - scene_nverts;
}

/* ---- textured triangles (wobbly windows) ----
 * UVs ride in the color attribute (.xy) and the texturing fragment
 * shader samples the desktop texture with them. Coordinates are given
 * in DESKTOP PIXELS (both position and texel), and the same
 * orientation convention as the plain scene applies automatically. */

static void scene_tex_vtx(float px, float py, float tx, float ty) {
    if (scene_tex_nverts >= SCENE_MAX_TEX_VERTS) return;
    float* v = &scene_tex_verts[scene_tex_nverts * 8];
    v[0] = -1.0f + 2.0f * px / (float)compiz_w;
    v[1] = compiz_flip ? (-1.0f + 2.0f * py / (float)compiz_h)
                       : ( 1.0f - 2.0f * py / (float)compiz_h);
    v[2] = scene_z;
    v[3] = 1.0f;
    /* Texture rows are stored mirrored under flip — mirror V to match */
    v[4] = tx / (float)compiz_w;
    v[5] = compiz_flip ? (((float)compiz_h - ty) / (float)compiz_h)
                       : (ty / (float)compiz_h);
    v[6] = 0.0f;
    v[7] = 1.0f;
    scene_tex_nverts++;
}

void virgl_scene_tex_tri(float px0, float py0, float tx0, float ty0,
                         float px1, float py1, float tx1, float ty1,
                         float px2, float py2, float tx2, float ty2) {
    scene_tex_vtx(px0, py0, tx0, ty0);
    scene_tex_vtx(px1, py1, tx1, ty1);
    scene_tex_vtx(px2, py2, tx2, ty2);
    scene_step_z();
}

bool virgl_scene_available(void) {
    return vctx.initialized && compiz_fb_res != 0;
}

/* Render the accumulated scene into the compositor framebuffer:
 * one batch = SET_SUB_CTX 0, CLEAR, vertex INLINE_WRITE into the
 * compositor VBO, SET_VERTEX_BUFFERS, DRAW_VBO. One round trip. */
bool virgl_scene_flush(void) {
    if (!vctx.initialized || !compiz_fb_res) return false;

    extern uint32_t virgl_pipeline_fs_plain(void);
    extern uint32_t virgl_pipeline_fs_tex(void);

    uint32_t vbytes = scene_nverts * 8 * 4;
    uint32_t tbytes = scene_tex_nverts * 8 * 4;
    if (vbytes + tbytes > compiz_vbo_size) {
        if (vbytes > compiz_vbo_size) vbytes = compiz_vbo_size;
        tbytes = compiz_vbo_size - vbytes;
    }
    uint32_t nverts  = vbytes / 32;
    uint32_t ntverts = (virgl_pipeline_fs_tex() != 0) ? (tbytes / 32) : 0;
    if (nverts == 0 && ntverts == 0) return true;

    uint32_t n = 0;
    scene_cmd[n++] = VIRGL_CMD_HDR(VIRGL_CCMD_SET_SUB_CTX, 0, 1);
    scene_cmd[n++] = 0;

    /* CLEAR color+depth (same encoding as virgl_cmd_clear) */
    scene_cmd[n++] = VIRGL_CMD_HDR(VIRGL_CCMD_CLEAR, 0, 8);
    scene_cmd[n++] = 4u | 1u;         /* PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH */
    scene_cmd[n++] = 0;               /* r = 0.0f */
    scene_cmd[n++] = 0;               /* g */
    scene_cmd[n++] = 0;               /* b */
    scene_cmd[n++] = f2u(1.0f);       /* a */
    {
        uint64_t dz = d2u(1.0);
        scene_cmd[n++] = (uint32_t)(dz & 0xFFFFFFFFu);
        scene_cmd[n++] = (uint32_t)(dz >> 32);
    }
    scene_cmd[n++] = 0;               /* stencil */

    /* Vertex data -> compositor VBO (RESOURCE_INLINE_WRITE):
     * plain scene verts followed by textured (wobble) verts */
    uint32_t total_bytes = vbytes + ntverts * 32;
    uint32_t data_words = total_bytes / 4;
    scene_cmd[n++] = VIRGL_CMD_HDR(VIRGL_CCMD_RESOURCE_INLINE_WRITE, 0, 11 + data_words);
    scene_cmd[n++] = compiz_vbo_res;
    scene_cmd[n++] = 0;               /* level */
    scene_cmd[n++] = 0;               /* usage */
    scene_cmd[n++] = 0;               /* stride */
    scene_cmd[n++] = 0;               /* layer_stride */
    scene_cmd[n++] = 0;               /* x */
    scene_cmd[n++] = 0;               /* y */
    scene_cmd[n++] = 0;               /* z */
    scene_cmd[n++] = total_bytes;     /* w = bytes for buffers */
    scene_cmd[n++] = 1;               /* h */
    scene_cmd[n++] = 1;               /* d */
    memcpy(&scene_cmd[n], scene_verts, vbytes);
    if (ntverts)
        memcpy((uint8_t*)&scene_cmd[n] + vbytes, scene_tex_verts, ntverts * 32);
    n += data_words;

    /* Bind vertex buffer (stride 32, offset 0) */
    scene_cmd[n++] = VIRGL_CMD_HDR(VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, 3);
    scene_cmd[n++] = 32;
    scene_cmd[n++] = 0;
    scene_cmd[n++] = compiz_vbo_res;

    /* DRAW_VBO (same 12-word layout as virgl_cmd_draw) */
    scene_cmd[n++] = VIRGL_CMD_HDR(VIRGL_CCMD_DRAW_VBO, 0, 12);
    scene_cmd[n++] = 0;                    /* start */
    scene_cmd[n++] = nverts;               /* count */
    scene_cmd[n++] = PIPE_PRIM_TRIANGLES;  /* mode */
    scene_cmd[n++] = 0;                    /* indexed */
    scene_cmd[n++] = 1;                    /* instance_count */
    scene_cmd[n++] = 0;                    /* index_bias */
    scene_cmd[n++] = 0;                    /* start_instance */
    scene_cmd[n++] = 0;                    /* primitive_restart */
    scene_cmd[n++] = 0;                    /* restart_index */
    scene_cmd[n++] = 0;                    /* min_index */
    scene_cmd[n++] = 0xFFFFFFFF;           /* max_index */
    scene_cmd[n++] = 0;                    /* cso */

    /* Wobbly-window pass: same VBO, texturing fragment shader, verts
     * appended after the plain scene's — drawn with start=nverts. */
    if (ntverts) {
        scene_cmd[n++] = VIRGL_CMD_HDR(VIRGL_CCMD_BIND_SHADER, 0, 2);
        scene_cmd[n++] = virgl_pipeline_fs_tex();
        scene_cmd[n++] = PIPE_SHADER_FRAGMENT;

        scene_cmd[n++] = VIRGL_CMD_HDR(VIRGL_CCMD_DRAW_VBO, 0, 12);
        scene_cmd[n++] = nverts;               /* start */
        scene_cmd[n++] = ntverts;              /* count */
        scene_cmd[n++] = PIPE_PRIM_TRIANGLES;
        scene_cmd[n++] = 0;
        scene_cmd[n++] = 1;
        scene_cmd[n++] = 0;
        scene_cmd[n++] = 0;
        scene_cmd[n++] = 0;
        scene_cmd[n++] = 0;
        scene_cmd[n++] = 0;
        scene_cmd[n++] = 0xFFFFFFFF;
        scene_cmd[n++] = 0;

        scene_cmd[n++] = VIRGL_CMD_HDR(VIRGL_CCMD_BIND_SHADER, 0, 2);
        scene_cmd[n++] = virgl_pipeline_fs_plain();
        scene_cmd[n++] = PIPE_SHADER_FRAGMENT;
    }

    return virgl_submit_cmd_buf(scene_cmd, n * 4);
}

/* Composite pass: GPU-rendered chrome (compositor fb) -> display, then the
 * CPU content rectangles (desktop texture) on top, then flush scanout. */
void virgl_compiz_compose(const virgl_rect_t* rects, uint32_t nrects) {
    if (!vctx.initialized || !compiz_fb_res) return;

    uint32_t cbuf[2 + 15 * 80];
    uint32_t n = 0;
    cbuf[n++] = VIRGL_CMD_HDR(VIRGL_CCMD_SET_SUB_CTX, 0, 1);
    cbuf[n++] = 0;

    /* 1) chrome: compositor fb -> display (full screen) */
    cbuf[n++] = VIRGL_CMD_HDR(VIRGL_CCMD_RESOURCE_COPY_REGION, 0, 13);
    cbuf[n++] = vctx.display_res_id;
    cbuf[n++] = 0; cbuf[n++] = 0; cbuf[n++] = 0; cbuf[n++] = 0;
    cbuf[n++] = compiz_fb_res;
    cbuf[n++] = 0; cbuf[n++] = 0; cbuf[n++] = 0; cbuf[n++] = 0;
    cbuf[n++] = compiz_w;
    cbuf[n++] = compiz_h;
    cbuf[n++] = 1;

    /* 2) stacked rects: CPU content (desktop texture) or GPU chrome (fb),
     * in caller-supplied bottom-to-top order so window stacking is right.
     * Screen-space rects are given by the GUI; when flip is on, every
     * source AND destination lives in mirrored rows, so mirror uniformly. */
    if (nrects > 78) nrects = 78;
    for (uint32_t i = 0; i < nrects; i++) {
        int x = rects[i].x, y = rects[i].y, w = rects[i].w, h = rects[i].h;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > (int)compiz_w) w = (int)compiz_w - x;
        if (y + h > (int)compiz_h) h = (int)compiz_h - y;
        if (w <= 0 || h <= 0) continue;
        uint32_t ry = compiz_flip ? (uint32_t)((int)compiz_h - y - h)
                                  : (uint32_t)y;
        cbuf[n++] = VIRGL_CMD_HDR(VIRGL_CCMD_RESOURCE_COPY_REGION, 0, 13);
        cbuf[n++] = vctx.display_res_id;
        cbuf[n++] = 0;
        cbuf[n++] = (uint32_t)x; cbuf[n++] = ry; cbuf[n++] = 0;
        cbuf[n++] = rects[i].src ? compiz_fb_res : compiz_tex_id;
        cbuf[n++] = 0;
        cbuf[n++] = (uint32_t)x; cbuf[n++] = ry; cbuf[n++] = 0;
        cbuf[n++] = (uint32_t)w;
        cbuf[n++] = (uint32_t)h;
        cbuf[n++] = 1;
    }

    if (!virgl_submit_cmd_buf(cbuf, n * 4)) {
        serial_printf("compiz: compose submit failed\n");
        return;
    }

    /* 3) flush scanout (full screen) */
    virtio_gpu_resource_flush_t flush;
    memset(&flush, 0, sizeof(flush));
    flush.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.resource_id = vctx.display_res_id;
    flush.r.x = 0;
    flush.r.y = 0;
    flush.r.width  = compiz_w;
    flush.r.height = compiz_h;
    gpu3d_cmd_ok(&flush, sizeof(flush));
}

/* Upload a dirty band of the CPU desktop into the desktop texture without
 * presenting (the compose pass presents). Rows are copied from `src` into
 * the staging buffer — mirrored vertically when flip is on, so content
 * lands upright on a flipped scanout. */
void virgl_compiz_upload_band(const uint32_t* src, uint32_t y0, uint32_t band_h) {
    if (!compiz_tex_id || !vctx.initialized || !compiz_stage || !src) return;
    if (y0 >= compiz_h) return;
    if (y0 + band_h > compiz_h) band_h = compiz_h - y0;
    if (band_h == 0) return;

    const uint32_t stride = (uint32_t)compiz_w * 4;

    uint32_t tex_y0;
    if (compiz_flip) {
        /* screen rows [y0, y0+band_h) -> texture rows mirrored */
        for (uint32_t j = 0; j < band_h; j++) {
            uint32_t sy = y0 + j;
            uint32_t ty = (uint32_t)compiz_h - 1 - sy;
            memcpy(compiz_stage + ty * compiz_w, src + sy * compiz_w, stride);
        }
        tex_y0 = (uint32_t)compiz_h - y0 - band_h;
    } else {
        memcpy(compiz_stage + y0 * compiz_w, src + y0 * compiz_w,
               (uint32_t)band_h * stride);
        tex_y0 = y0;
    }

    virtio_gpu_transfer_host_3d_t xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.hdr.type   = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    xfer.hdr.ctx_id = vctx.ctx_id;
    xfer.box.x = 0;
    xfer.box.y = tex_y0;
    xfer.box.z = 0;
    xfer.box.w = compiz_w;
    xfer.box.h = band_h;
    xfer.box.d = 1;
    xfer.offset       = (uint64_t)tex_y0 * stride;
    xfer.resource_id  = compiz_tex_id;
    xfer.level        = 0;
    xfer.stride       = stride;
    xfer.layer_stride = 0;
    gpu3d_cmd_ok(&xfer, sizeof(xfer));
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