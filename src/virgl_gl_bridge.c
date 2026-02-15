#include "virgl.h"
#include "virgl_pipeline.h"
#include "minigl.h"
#include "serial.h"
#include "heap.h"

/*
 * MiniGL → Virgl Bridge
 *
 * This file replaces minigl.c's software rasterizer with GPU-accelerated
 * rendering via virgl. The idea:
 *
 *   YOUR CODE (unchanged):
 *     glBegin(GL_TRIANGLES);
 *       glColor3f(1, 0, 0);
 *       glVertex3f(-1, -1, 0);
 *       ...
 *     glEnd();
 *
 *   OLD PATH (minigl.c — CPU):
 *     glEnd() → transform_vertex() → rasterize_triangle() → pixel loop
 *     All on the CPU. Slow for large scenes.
 *
 *   NEW PATH (this file — GPU):
 *     glEnd() → pack vertices into buffer → build MVP matrix →
 *     virgl_cmd_draw() → submit to host GPU → done.
 *     Host GPU does the rasterization in hardware.
 *
 * HOW TO USE:
 *   1. Call virgl_gl_init() instead of glInit() when virgl is available
 *   2. All other gl* calls work the same
 *   3. Call virgl_gl_swap() instead of blitting the pixbuf yourself
 *
 * BUILDING:
 *   - If virgl is available:  compile this file, DON'T compile minigl.c
 *   - If virgl not available: compile minigl.c as before (software fallback)
 *   - OR: compile both and switch at runtime (see bottom of file)
 */

#define PI 3.14159265358979f
#define MAX_MATRIX_STACK 16
#define MAX_BATCH_VERTS 4096

/* ---- Import minigl's math (shared) ---- */
extern float gl_sin(float x);
extern float gl_cos(float x);
extern float gl_sqrt(float x);

/* ---- 4x4 Matrix (same as minigl) ---- */
typedef struct { float m[16]; } mat4_t;

static void mat4_identity(mat4_t* M) {
    for (int i = 0; i < 16; i++) M->m[i] = 0;
    M->m[0] = M->m[5] = M->m[10] = M->m[15] = 1.0f;
}

static void mat4_mul(mat4_t* out, const mat4_t* A, const mat4_t* B) {
    mat4_t R;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += A->m[r * 4 + k] * B->m[k * 4 + c];
            R.m[r * 4 + c] = s;
        }
    *out = R;
}

/* ---- Packed vertex for GPU upload ---- */
/* Each vertex sent to the GPU: position(3) + color(4) = 7 floats = 28 bytes */
typedef struct {
    float x, y, z;
    float r, g, b, a;
} gpu_vertex_t;

#define GPU_VERTEX_STRIDE sizeof(gpu_vertex_t)  /* 28 bytes */

/* ---- GL Context (GPU-accelerated version) ---- */
static struct {
    bool     active;
    uint16_t width, height;

    /* Matrix stacks (same as minigl — we compute MVP on CPU, send to GPU) */
    int      matrix_mode;
    mat4_t   modelview;
    mat4_t   projection;
    mat4_t   mv_stack[MAX_MATRIX_STACK];
    mat4_t   proj_stack[MAX_MATRIX_STACK];
    int      mv_top, proj_top;

    /* Viewport */
    int vp_x, vp_y, vp_w, vp_h;

    /* Current vertex attributes */
    float    cur_color[4];

    /* Vertex batch (collected between glBegin/glEnd) */
    gpu_vertex_t batch[MAX_BATCH_VERTS];
    int          batch_count;
    int          prim_mode;

    /* State */
    bool     depth_test;
    float    clear_r, clear_g, clear_b, clear_a;

    /* Pipeline state created? */
    bool     pipeline_ready;
} gctx;

/* Forward declaration */
static void virgl_gl_setup_pipeline(void);

/* ===== Initialization ===== */

bool virgl_gl_init(uint16_t width, uint16_t height) {
    memset(&gctx, 0, sizeof(gctx));

    if (!virgl_init()) {
        serial_printf("virgl_gl: virgl_init failed, falling back to software\n");
        return false;
    }

    if (!virgl_setup_framebuffer(width, height)) {
        serial_printf("virgl_gl: framebuffer setup failed\n");
        return false;
    }

    gctx.width = width;
    gctx.height = height;
    gctx.active = true;

    mat4_identity(&gctx.modelview);
    mat4_identity(&gctx.projection);

    gctx.vp_x = 0; gctx.vp_y = 0;
    gctx.vp_w = width; gctx.vp_h = height;

    gctx.cur_color[0] = gctx.cur_color[1] = gctx.cur_color[2] = 1.0f;
    gctx.cur_color[3] = 1.0f;
    gctx.depth_test = true;

    /*
     * Set up the GPU pipeline state objects (done once).
     *
     * In virgl, you need to create various state objects:
     *   - Blend state (how colors are combined)
     *   - Rasterizer state (fill mode, cull mode, etc.)
     *   - Depth-stencil-alpha state (z-test settings)
     *   - Shaders (vertex + fragment)
     *   - Vertex element layout (describes vertex format)
     *   - Surface objects (for framebuffer attachment)
     *
     * These are created once and bound before drawing.
     */
    virgl_gl_setup_pipeline();

    serial_printf("virgl_gl: initialized %ux%u GPU-accelerated rendering\n",
                  width, height);
    return true;
}

/* ===== Pipeline Setup ===== */
/*
 * This creates the GPU state objects needed for rendering.
 * These correspond to Gallium3D CSOs (Constant State Objects).
 *
 * In a full implementation, you'd create TGSI shader programs.
 * For now, we use the simplest possible shaders:
 *   - Vertex shader: pass-through (just applies MVP matrix from constant buffer)
 *   - Fragment shader: output vertex color
 */
static void virgl_gl_setup_pipeline(void) {
    if (!virgl_setup_pipeline_state()) {
        serial_printf("virgl_gl: pipeline setup FAILED!\n");
        gctx.pipeline_ready = false;
        return;
    }

    gctx.pipeline_ready = true;
    serial_printf("virgl_gl: GPU pipeline ready for 3D rendering\n");
}

/* ===== GL State Functions (same API as minigl) ===== */

static mat4_t* current_matrix(void) {
    return (gctx.matrix_mode == GL_PROJECTION) ? &gctx.projection : &gctx.modelview;
}

void virgl_glMatrixMode(int mode) { gctx.matrix_mode = mode; }
void virgl_glLoadIdentity(void) { mat4_identity(current_matrix()); }

void virgl_glPushMatrix(void) {
    if (gctx.matrix_mode == GL_PROJECTION) {
        if (gctx.proj_top < MAX_MATRIX_STACK - 1)
            gctx.proj_stack[gctx.proj_top++] = gctx.projection;
    } else {
        if (gctx.mv_top < MAX_MATRIX_STACK - 1)
            gctx.mv_stack[gctx.mv_top++] = gctx.modelview;
    }
}

void virgl_glPopMatrix(void) {
    if (gctx.matrix_mode == GL_PROJECTION) {
        if (gctx.proj_top > 0) gctx.projection = gctx.proj_stack[--gctx.proj_top];
    } else {
        if (gctx.mv_top > 0) gctx.modelview = gctx.mv_stack[--gctx.mv_top];
    }
}

void virgl_glTranslatef(float x, float y, float z) {
    mat4_t T; mat4_identity(&T);
    T.m[3] = x; T.m[7] = y; T.m[11] = z;
    mat4_t* cur = current_matrix();
    mat4_t R; mat4_mul(&R, cur, &T);
    *cur = R;
}

void virgl_glRotatef(float angle, float ax, float ay, float az) {
    float rad = angle * PI / 180.0f;
    float c = gl_cos(rad), s = gl_sin(rad);
    float len = gl_sqrt(ax*ax + ay*ay + az*az);
    if (len < 1e-7f) return;
    ax /= len; ay /= len; az /= len;
    float nc = 1 - c;

    mat4_t Rot; mat4_identity(&Rot);
    Rot.m[0]  = ax*ax*nc + c;     Rot.m[1]  = ax*ay*nc - az*s; Rot.m[2]  = ax*az*nc + ay*s;
    Rot.m[4]  = ay*ax*nc + az*s;  Rot.m[5]  = ay*ay*nc + c;    Rot.m[6]  = ay*az*nc - ax*s;
    Rot.m[8]  = az*ax*nc - ay*s;  Rot.m[9]  = az*ay*nc + ax*s; Rot.m[10] = az*az*nc + c;

    mat4_t* cur = current_matrix();
    mat4_t R; mat4_mul(&R, cur, &Rot);
    *cur = R;
}

void virgl_glScalef(float x, float y, float z) {
    mat4_t S; mat4_identity(&S);
    S.m[0] = x; S.m[5] = y; S.m[10] = z;
    mat4_t* cur = current_matrix();
    mat4_t R; mat4_mul(&R, cur, &S);
    *cur = R;
}

void virgl_gluPerspective(float fovy, float aspect, float zn, float zf) {
    float rad = fovy * PI / 180.0f;
    float f = gl_cos(rad/2) / gl_sin(rad/2);
    mat4_t P; memset(&P, 0, sizeof(P));
    P.m[0]  = f / aspect;
    P.m[5]  = f;
    P.m[10] = (zf + zn) / (zn - zf);
    P.m[11] = (2 * zf * zn) / (zn - zf);
    P.m[14] = -1;
    mat4_t* cur = current_matrix();
    mat4_t R; mat4_mul(&R, cur, &P);
    *cur = R;
}

void virgl_glViewport(int x, int y, int w, int h) {
    gctx.vp_x = x; gctx.vp_y = y; gctx.vp_w = w; gctx.vp_h = h;
}

void virgl_glClearColor(float r, float g, float b, float a) {
    gctx.clear_r = r; gctx.clear_g = g; gctx.clear_b = b; gctx.clear_a = a;
}

void virgl_glClear(int mask) {
    virgl_cmd_begin();

    uint32_t buffers = 0;
    if (mask & GL_COLOR_BUFFER_BIT) buffers |= 0x1;  /* PIPE_CLEAR_COLOR */
    if (mask & GL_DEPTH_BUFFER_BIT) buffers |= 0x2;  /* PIPE_CLEAR_DEPTH */

    virgl_cmd_clear(buffers, gctx.clear_r, gctx.clear_g, gctx.clear_b,
                    gctx.clear_a, 1.0, 0);
    virgl_cmd_submit();
}

void virgl_glEnable(int cap) {
    if (cap == GL_DEPTH_TEST) gctx.depth_test = true;
    /* Other caps require recreating DSA/rasterizer state objects */
}

void virgl_glDisable(int cap) {
    if (cap == GL_DEPTH_TEST) gctx.depth_test = false;
}

/* ===== Vertex Submission ===== */

void virgl_glBegin(int mode) {
    gctx.prim_mode = mode;
    gctx.batch_count = 0;
}

void virgl_glColor3f(float r, float g, float b) {
    gctx.cur_color[0] = r; gctx.cur_color[1] = g;
    gctx.cur_color[2] = b; gctx.cur_color[3] = 1.0f;
}

void virgl_glColor4f(float r, float g, float b, float a) {
    gctx.cur_color[0] = r; gctx.cur_color[1] = g;
    gctx.cur_color[2] = b; gctx.cur_color[3] = a;
}

void virgl_glNormal3f(float nx, float ny, float nz) {
    /* For the basic virgl path, normals are handled by the vertex shader.
     * We'd need to extend the vertex format to include normals. */
    (void)nx; (void)ny; (void)nz;
}

void virgl_glVertex3f(float x, float y, float z) {
    if (gctx.batch_count >= MAX_BATCH_VERTS) return;

    gpu_vertex_t* v = &gctx.batch[gctx.batch_count++];
    v->x = x; v->y = y; v->z = z;
    v->r = gctx.cur_color[0];
    v->g = gctx.cur_color[1];
    v->b = gctx.cur_color[2];
    v->a = gctx.cur_color[3];
}

void virgl_glVertex2f(float x, float y) {
    virgl_glVertex3f(x, y, 0.0f);
}

void virgl_glEnd(void) {
    if (gctx.batch_count == 0) return;

    /* Convert GL primitive mode to Gallium PIPE_PRIM */
    uint32_t pipe_prim;
    switch (gctx.prim_mode) {
        case GL_TRIANGLES:      pipe_prim = PIPE_PRIM_TRIANGLES; break;
        case GL_TRIANGLE_FAN:   pipe_prim = PIPE_PRIM_TRIANGLE_FAN; break;
        case GL_TRIANGLE_STRIP: pipe_prim = PIPE_PRIM_TRIANGLE_STRIP; break;
        case GL_QUADS:          pipe_prim = PIPE_PRIM_QUADS; break;
        case GL_LINES:          pipe_prim = PIPE_PRIM_LINES; break;
        case GL_LINE_STRIP:     pipe_prim = PIPE_PRIM_LINE_STRIP; break;
        case GL_POLYGON:        pipe_prim = PIPE_PRIM_TRIANGLE_FAN; break;
        default:                pipe_prim = PIPE_PRIM_TRIANGLES; break;
    }

    /* Build and submit the draw command buffer */
    virgl_cmd_begin();

    /* 1. Upload MVP matrix as constant buffer for vertex shader */
    mat4_t mvp;
    mat4_mul(&mvp, &gctx.projection, &gctx.modelview);
    virgl_cmd_set_constant_buffer(PIPE_SHADER_VERTEX, mvp.m, 16);

    /* 2. Set viewport */
   // virgl_cmd_set_viewport((float)gctx.vp_x, (float)gctx.vp_y,
 //                          (float)gctx.vp_w, (float)gctx.vp_h,
     //                      0.0f, 1.0f);

    /* 3. Upload vertex data */
    virgl_upload_vertices((const float*)gctx.batch,
                          gctx.batch_count * (sizeof(gpu_vertex_t) / sizeof(float)));

    /* 4. Bind vertex buffer */
    virgl_cmd_set_vertex_buffer(GPU_VERTEX_STRIDE, 0);

    /* 5. Draw */
    virgl_cmd_draw(pipe_prim, 0, gctx.batch_count);

    /* 6. Submit to GPU */
    virgl_cmd_submit();

    gctx.batch_count = 0;
}

/* ===== Present / SwapBuffers ===== */

void virgl_gl_swap(void) {
    virgl_present();
}

/* ===== Shutdown ===== */

void virgl_gl_close(void) {
    virgl_shutdown();
    gctx.active = false;
}

/*
 * ================================================================
 *  RUNTIME SWITCHING — Use this to pick GPU vs CPU at runtime
 * ================================================================
 *
 * In your gui.c or wherever you call minigl, do this:
 *
 *   // At initialization:
 *   bool use_gpu = false;
 *   if (virgl_available()) {
 *       use_gpu = virgl_gl_init(GL_WIN_W, GL_WIN_H);
 *   }
 *   if (!use_gpu) {
 *       glInit(gl_pixbuf, GL_WIN_W, GL_WIN_H);  // software fallback
 *   }
 *
 *   // In your draw function:
 *   if (use_gpu) {
 *       // These call virgl_gl* versions which submit to GPU
 *       virgl_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
 *       virgl_glMatrixMode(GL_PROJECTION);
 *       virgl_glLoadIdentity();
 *       virgl_gluPerspective(60, aspect, 0.1f, 100);
 *       // ... same gl calls but prefixed with virgl_ ...
 *       virgl_glBegin(GL_TRIANGLES);
 *       virgl_glColor3f(1, 0, 0);
 *       virgl_glVertex3f(0, 1, 0);
 *       // ...
 *       virgl_glEnd();
 *       virgl_gl_swap();  // display the result
 *   } else {
 *       // CPU software path (existing minigl)
 *       glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
 *       // ... existing code unchanged ...
 *   }
 *
 * ================================================================
 *  ALTERNATIVE: Function pointer dispatch (transparent switching)
 * ================================================================
 *
 * If you want the SAME code to work with both backends without
 * changing every call site, use function pointers:
 *
 *   // In a new file: gl_dispatch.h
 *   typedef struct {
 *       void (*clear)(int mask);
 *       void (*matrixMode)(int mode);
 *       void (*loadIdentity)(void);
 *       void (*translatef)(float x, float y, float z);
 *       void (*rotatef)(float angle, float x, float y, float z);
 *       void (*begin)(int mode);
 *       void (*end)(void);
 *       void (*vertex3f)(float x, float y, float z);
 *       void (*color3f)(float r, float g, float b);
 *       void (*swap)(void);
 *       // ... etc for all gl functions
 *   } gl_dispatch_t;
 *
 *   extern gl_dispatch_t gl;
 *
 *   // In gl_dispatch.c:
 *   gl_dispatch_t gl;
 *
 *   void gl_init_dispatch(bool use_gpu) {
 *       if (use_gpu) {
 *           gl.clear = virgl_glClear;
 *           gl.begin = virgl_glBegin;
 *           gl.end = virgl_glEnd;
 *           gl.vertex3f = virgl_glVertex3f;
 *           // ...
 *       } else {
 *           gl.clear = glClear;
 *           gl.begin = glBegin;
 *           gl.end = glEnd;
 *           gl.vertex3f = glVertex3f;
 *           // ...
 *       }
 *   }
 *
 *   // Then your draw code just uses:
 *   gl.begin(GL_TRIANGLES);
 *   gl.vertex3f(0, 1, 0);
 *   gl.end();
 *   // Works with either backend!
 */
