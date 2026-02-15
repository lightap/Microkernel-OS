#include "minigl.h"
#include "heap.h"

/* ================================================================
 * MiniGL — Software OpenGL 1.1 rasterizer for MicroKernel
 * Supports: matrix stack, perspective, triangles/quads,
 *           z-buffer, smooth shading, basic lighting
 * ================================================================ */

#define MAX_MATRIX_STACK 16
#define MAX_VERTS 1024
#define PI 3.14159265358979f

/* ---- Math ---- */
static float fabsf_(float x) { return x < 0 ? -x : x; }

float gl_sqrt(float x) {
    if (x <= 0) return 0;
    float r = x;
    for (int i = 0; i < 15; i++) r = 0.5f * (r + x / r);
    return r;
}

float gl_sin(float x) {
    /* Reduce to [-PI, PI] */
    while (x > PI) x -= 2 * PI;
    while (x < -PI) x += 2 * PI;
    /* Pade-like approximation */
    float x2 = x * x;
    float x3 = x2 * x;
    float x5 = x3 * x2;
    float x7 = x5 * x2;
    return x - x3 / 6.0f + x5 / 120.0f - x7 / 5040.0f;
}

float gl_cos(float x) { return gl_sin(x + PI / 2.0f); }
float gl_tan(float x) { float c = gl_cos(x); return (fabsf_(c) < 1e-7f) ? 1e7f : gl_sin(x) / c; }

static float rsqrt_(float x) {
    if (x <= 0) return 0;
    return 1.0f / gl_sqrt(x);
}

/* ---- 4x4 Matrix ---- */
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

static void mat4_transform(const mat4_t* M, const float in[4], float out[4]) {
    for (int r = 0; r < 4; r++) {
        out[r] = 0;
        for (int c = 0; c < 4; c++)
            out[r] += M->m[r * 4 + c] * in[c];
    }
}

/* Transform normal (upper-left 3x3 only, no translation) */
static void mat4_transform_normal(const mat4_t* M, const float in[3], float out[3]) {
    for (int r = 0; r < 3; r++) {
        out[r] = 0;
        for (int c = 0; c < 3; c++)
            out[r] += M->m[r * 4 + c] * in[c];
    }
    float len = gl_sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]);
    if (len > 1e-7f) { out[0] /= len; out[1] /= len; out[2] /= len; }
}

/* ---- Vertex ---- */
typedef struct {
    float pos[4];    /* Object-space xyzw */
    float color[4];  /* RGBA */
    float normal[3]; /* Object-space normal */
    /* Transformed */
    float screen[3]; /* Screen-space x, y, z(depth) */
    float shade[4];  /* Final lit color */
} vertex_t;

/* ---- GL Context ---- */
static struct {
    uint32_t* fb;
    float*    zbuf;
    uint16_t  width, height;
    float     clear_r, clear_g, clear_b;

    /* Matrix stacks */
    int      matrix_mode;  /* GL_MODELVIEW or GL_PROJECTION */
    mat4_t   modelview;
    mat4_t   projection;
    mat4_t   mv_stack[MAX_MATRIX_STACK];
    mat4_t   proj_stack[MAX_MATRIX_STACK];
    int      mv_top, proj_top;

    /* Viewport */
    int vp_x, vp_y, vp_w, vp_h;

    /* Current vertex attributes */
    float    cur_color[4];
    float    cur_normal[3];

    /* Primitive assembly */
    int      prim_mode;
    vertex_t verts[MAX_VERTS];
    int      vert_count;

    /* Caps */
    bool     depth_test;
    bool     lighting;
    bool     light0_on;
    bool     cull_face;
    bool     wireframe;

    /* Light 0 */
    float    light0_pos[4];
    float    light0_ambient[4];
    float    light0_diffuse[4];
} ctx;

/* ---- Init ---- */
void glInit(uint32_t* framebuffer, uint16_t width, uint16_t height) {
    /* Free old zbuf if re-initializing */
    float* old_zbuf = ctx.zbuf;
    memset(&ctx, 0, sizeof(ctx));
    if (old_zbuf) kfree(old_zbuf);
    ctx.fb = framebuffer;
    ctx.width = width;
    ctx.height = height;

    ctx.zbuf = (float*)kmalloc((uint32_t)width * height * sizeof(float));

    mat4_identity(&ctx.modelview);
    mat4_identity(&ctx.projection);
    ctx.mv_top = ctx.proj_top = 0;

    ctx.vp_x = 0; ctx.vp_y = 0;
    ctx.vp_w = width; ctx.vp_h = height;

    ctx.cur_color[0] = ctx.cur_color[1] = ctx.cur_color[2] = ctx.cur_color[3] = 1.0f;
    ctx.cur_normal[1] = 1.0f; /* default up */

    ctx.depth_test = true;
    ctx.lighting = false;
    ctx.light0_on = false;
    ctx.cull_face = false;

    /* Default light */
    ctx.light0_pos[0] = 0; ctx.light0_pos[1] = 0;
    ctx.light0_pos[2] = 1; ctx.light0_pos[3] = 0;
    ctx.light0_ambient[0] = ctx.light0_ambient[1] = ctx.light0_ambient[2] = 0.2f;
    ctx.light0_ambient[3] = 1.0f;
    ctx.light0_diffuse[0] = ctx.light0_diffuse[1] = ctx.light0_diffuse[2] = 0.8f;
    ctx.light0_diffuse[3] = 1.0f;
}

void glClose(void) {
    if (ctx.zbuf) { kfree(ctx.zbuf); ctx.zbuf = NULL; }
}

/* Switch render target without reallocating zbuf (if same dimensions) */
void glSetTarget(uint32_t* framebuffer, uint16_t width, uint16_t height) {
    if (ctx.zbuf && ctx.width == width && ctx.height == height) {
        ctx.fb = framebuffer;
        return;
    }
    /* Dimensions changed — full reinit */
    glInit(framebuffer, width, height);
}

/* ---- State ---- */
void glEnable(int cap) {
    if (cap == GL_DEPTH_TEST) ctx.depth_test = true;
    else if (cap == GL_LIGHTING) ctx.lighting = true;
    else if (cap == GL_LIGHT0) ctx.light0_on = true;
    else if (cap == GL_CULL_FACE) ctx.cull_face = true;
}
void glDisable(int cap) {
    if (cap == GL_DEPTH_TEST) ctx.depth_test = false;
    else if (cap == GL_LIGHTING) ctx.lighting = false;
    else if (cap == GL_LIGHT0) ctx.light0_on = false;
    else if (cap == GL_CULL_FACE) ctx.cull_face = false;
}

void glPolygonMode(int mode) {
    ctx.wireframe = (mode == GL_LINE);
}

void glClearColor(float r, float g, float b, float a) {
    (void)a;
    ctx.clear_r = r; ctx.clear_g = g; ctx.clear_b = b;
}

void glClear(int mask) {
    uint32_t n = (uint32_t)ctx.width * ctx.height;
    if (mask & GL_COLOR_BUFFER_BIT) {
        uint8_t cr = (uint8_t)(ctx.clear_r * 255);
        uint8_t cg = (uint8_t)(ctx.clear_g * 255);
        uint8_t cb = (uint8_t)(ctx.clear_b * 255);
        uint32_t col = ((uint32_t)cr << 16) | ((uint32_t)cg << 8) | cb;
        for (uint32_t i = 0; i < n; i++) ctx.fb[i] = col;
    }
    if (mask & GL_DEPTH_BUFFER_BIT) {
        /* Use a large positive value (far plane) */
        for (uint32_t i = 0; i < n; i++) ctx.zbuf[i] = 1e30f;
    }
}

void glViewport(int x, int y, int w, int h) {
    ctx.vp_x = x; ctx.vp_y = y; ctx.vp_w = w; ctx.vp_h = h;
}

/* ---- Matrix ---- */
static mat4_t* current_matrix(void) {
    return (ctx.matrix_mode == GL_PROJECTION) ? &ctx.projection : &ctx.modelview;
}

void glMatrixMode(int mode) { ctx.matrix_mode = mode; }
void glLoadIdentity(void) { mat4_identity(current_matrix()); }

void glPushMatrix(void) {
    if (ctx.matrix_mode == GL_PROJECTION) {
        if (ctx.proj_top < MAX_MATRIX_STACK - 1) {
            ctx.proj_stack[ctx.proj_top++] = ctx.projection;
        }
    } else {
        if (ctx.mv_top < MAX_MATRIX_STACK - 1) {
            ctx.mv_stack[ctx.mv_top++] = ctx.modelview;
        }
    }
}
void glPopMatrix(void) {
    if (ctx.matrix_mode == GL_PROJECTION) {
        if (ctx.proj_top > 0) ctx.projection = ctx.proj_stack[--ctx.proj_top];
    } else {
        if (ctx.mv_top > 0) ctx.modelview = ctx.mv_stack[--ctx.mv_top];
    }
}

void glMultMatrixf(const float* m) {
    mat4_t M;
    for (int i = 0; i < 16; i++) M.m[i] = m[i];
    mat4_t* cur = current_matrix();
    mat4_t R;
    mat4_mul(&R, cur, &M);
    *cur = R;
}

void glTranslatef(float x, float y, float z) {
    mat4_t T;
    mat4_identity(&T);
    T.m[3] = x; T.m[7] = y; T.m[11] = z;
    mat4_t* cur = current_matrix();
    mat4_t R;
    mat4_mul(&R, cur, &T);
    *cur = R;
}

void glRotatef(float angle_deg, float ax, float ay, float az) {
    float a = angle_deg * PI / 180.0f;
    float c = gl_cos(a), s = gl_sin(a);
    float len = gl_sqrt(ax*ax + ay*ay + az*az);
    if (len < 1e-7f) return;
    ax /= len; ay /= len; az /= len;
    float nc = 1.0f - c;

    mat4_t R;
    mat4_identity(&R);
    R.m[0]  = ax*ax*nc + c;     R.m[1]  = ax*ay*nc - az*s;  R.m[2]  = ax*az*nc + ay*s;
    R.m[4]  = ay*ax*nc + az*s;  R.m[5]  = ay*ay*nc + c;     R.m[6]  = ay*az*nc - ax*s;
    R.m[8]  = az*ax*nc - ay*s;  R.m[9]  = az*ay*nc + ax*s;  R.m[10] = az*az*nc + c;

    mat4_t* cur = current_matrix();
    mat4_t out;
    mat4_mul(&out, cur, &R);
    *cur = out;
}

void glScalef(float x, float y, float z) {
    mat4_t S;
    mat4_identity(&S);
    S.m[0] = x; S.m[5] = y; S.m[10] = z;
    mat4_t* cur = current_matrix();
    mat4_t R;
    mat4_mul(&R, cur, &S);
    *cur = R;
}

void gluPerspective(float fovy, float aspect, float znear, float zfar) {
    float f = 1.0f / gl_tan(fovy * PI / 360.0f);
    mat4_t P;
    memset(&P, 0, sizeof(P));
    P.m[0]  = f / aspect;
    P.m[5]  = f;
    P.m[10] = (zfar + znear) / (znear - zfar);
    P.m[11] = (2.0f * zfar * znear) / (znear - zfar);
    P.m[14] = -1.0f;

    mat4_t* cur = current_matrix();
    mat4_t R;
    mat4_mul(&R, cur, &P);
    *cur = R;
}

void glOrtho(float l, float r, float b, float t, float n, float f) {
    mat4_t O;
    memset(&O, 0, sizeof(O));
    O.m[0]  = 2.0f / (r - l);
    O.m[5]  = 2.0f / (t - b);
    O.m[10] = -2.0f / (f - n);
    O.m[3]  = -(r + l) / (r - l);
    O.m[7]  = -(t + b) / (t - b);
    O.m[11] = -(f + n) / (f - n);
    O.m[15] = 1.0f;

    mat4_t* cur = current_matrix();
    mat4_t R;
    mat4_mul(&R, cur, &O);
    *cur = R;
}

void gluLookAt(float ex, float ey, float ez,
               float cx, float cy, float cz,
               float ux, float uy, float uz) {
    float fx = cx - ex, fy = cy - ey, fz = cz - ez;
    float fl = rsqrt_(fx*fx + fy*fy + fz*fz);
    fx *= fl; fy *= fl; fz *= fl;

    /* s = f x up */
    float sx = fy*uz - fz*uy, sy = fz*ux - fx*uz, sz = fx*uy - fy*ux;
    float sl = rsqrt_(sx*sx + sy*sy + sz*sz);
    sx *= sl; sy *= sl; sz *= sl;

    /* u = s x f */
    float uux = sy*fz - sz*fy, uuy = sz*fx - sx*fz, uuz = sx*fy - sy*fx;

    mat4_t M;
    mat4_identity(&M);
    M.m[0] = sx;  M.m[1] = sy;  M.m[2] = sz;
    M.m[4] = uux; M.m[5] = uuy; M.m[6] = uuz;
    M.m[8] = -fx; M.m[9] = -fy; M.m[10] = -fz;

    mat4_t* cur = current_matrix();
    mat4_t R;
    mat4_mul(&R, cur, &M);
    *cur = R;

    glTranslatef(-ex, -ey, -ez);
}

/* ---- Lighting ---- */
void glLightfv(int light, int param, const float* v) {
    if (light != GL_LIGHT0) return;
    float* dst = NULL;
    if (param == GL_POSITION)  dst = ctx.light0_pos;
    else if (param == GL_AMBIENT) dst = ctx.light0_ambient;
    else if (param == GL_DIFFUSE) dst = ctx.light0_diffuse;
    if (dst) { dst[0] = v[0]; dst[1] = v[1]; dst[2] = v[2]; dst[3] = v[3]; }
}

/* ---- Vertex submission ---- */
void glBegin(int mode) {
    ctx.prim_mode = mode;
    ctx.vert_count = 0;
}

void glColor3f(float r, float g, float b) {
    ctx.cur_color[0] = r; ctx.cur_color[1] = g;
    ctx.cur_color[2] = b; ctx.cur_color[3] = 1.0f;
}
void glColor4f(float r, float g, float b, float a) {
    ctx.cur_color[0] = r; ctx.cur_color[1] = g;
    ctx.cur_color[2] = b; ctx.cur_color[3] = a;
}
void glNormal3f(float nx, float ny, float nz) {
    ctx.cur_normal[0] = nx; ctx.cur_normal[1] = ny; ctx.cur_normal[2] = nz;
}

void glVertex3f(float x, float y, float z) {
    if (ctx.vert_count >= MAX_VERTS) return;
    vertex_t* v = &ctx.verts[ctx.vert_count++];
    v->pos[0] = x; v->pos[1] = y; v->pos[2] = z; v->pos[3] = 1.0f;
    v->color[0] = ctx.cur_color[0]; v->color[1] = ctx.cur_color[1];
    v->color[2] = ctx.cur_color[2]; v->color[3] = ctx.cur_color[3];
    v->normal[0] = ctx.cur_normal[0]; v->normal[1] = ctx.cur_normal[1];
    v->normal[2] = ctx.cur_normal[2];
}

void glVertex2f(float x, float y) { glVertex3f(x, y, 0.0f); }

/* ---- Rasterizer ---- */
static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
static inline int min3i(int a, int b, int c) { int m = a < b ? a : b; return m < c ? m : c; }
static inline int max3i(int a, int b, int c) { int m = a > b ? a : b; return m > c ? m : c; }
static inline float edge(float ax, float ay, float bx, float by, float cx, float cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

static void rasterize_line(vertex_t* v0, vertex_t* v1);

static void compute_lighting(vertex_t* v) {
    if (!ctx.lighting || !ctx.light0_on) {
        v->shade[0] = v->color[0]; v->shade[1] = v->color[1];
        v->shade[2] = v->color[2]; v->shade[3] = v->color[3];
        return;
    }

    /* Transform normal to eye space */
    float en[3];
    mat4_transform_normal(&ctx.modelview, v->normal, en);

    /* Light direction in eye space */
    float ld[3];
    if (ctx.light0_pos[3] == 0.0f) {
        /* Directional */
        ld[0] = ctx.light0_pos[0]; ld[1] = ctx.light0_pos[1]; ld[2] = ctx.light0_pos[2];
    } else {
        /* Point light: transform position, compute direction */
        float lp[4], lpe[4];
        lp[0] = ctx.light0_pos[0]; lp[1] = ctx.light0_pos[1];
        lp[2] = ctx.light0_pos[2]; lp[3] = ctx.light0_pos[3];
        mat4_transform(&ctx.modelview, lp, lpe);
        float ep[4]; mat4_transform(&ctx.modelview, v->pos, ep);
        ld[0] = lpe[0] - ep[0]; ld[1] = lpe[1] - ep[1]; ld[2] = lpe[2] - ep[2];
    }
    float ll = rsqrt_(ld[0]*ld[0] + ld[1]*ld[1] + ld[2]*ld[2]);
    ld[0] *= ll; ld[1] *= ll; ld[2] *= ll;

    float ndotl = en[0]*ld[0] + en[1]*ld[1] + en[2]*ld[2];
    if (ndotl < 0) ndotl = 0;

    for (int i = 0; i < 3; i++) {
        float c = ctx.light0_ambient[i] * v->color[i] +
                  ctx.light0_diffuse[i] * v->color[i] * ndotl;
        v->shade[i] = clampf(c, 0.0f, 1.0f);
    }
    v->shade[3] = v->color[3];
}

static void transform_vertex(vertex_t* v) {
    /* Model-view transform */
    float eye[4];
    mat4_transform(&ctx.modelview, v->pos, eye);

    /* Projection */
    float clip[4];
    mat4_transform(&ctx.projection, eye, clip);

    /* Perspective divide */
    if (fabsf_(clip[3]) < 1e-7f) clip[3] = 1e-7f;
    float inv_w = 1.0f / clip[3];
    float ndc_x = clip[0] * inv_w;
    float ndc_y = clip[1] * inv_w;
    float ndc_z = clip[2] * inv_w;

    /* Viewport transform */
    v->screen[0] = (ndc_x * 0.5f + 0.5f) * ctx.vp_w + ctx.vp_x;
    v->screen[1] = (1.0f - (ndc_y * 0.5f + 0.5f)) * ctx.vp_h + ctx.vp_y; /* Y-flip */
    v->screen[2] = ndc_z; /* depth: -1 near, 1 far */

    /* Compute lighting */
    compute_lighting(v);
}

static void rasterize_triangle(vertex_t* v0, vertex_t* v1, vertex_t* v2) {
    /* Wireframe mode: draw edges only */
    if (ctx.wireframe) {
        rasterize_line(v0, v1);
        rasterize_line(v1, v2);
        rasterize_line(v2, v0);
        return;
    }

    float x0 = v0->screen[0], y0 = v0->screen[1];
    float x1 = v1->screen[0], y1 = v1->screen[1];
    float x2 = v2->screen[0], y2 = v2->screen[1];

    /* Backface cull: Y-flip in viewport inverts winding,
     * so front-facing (CCW in clip space) = negative area in screen space */
    float area = edge(x0, y0, x1, y1, x2, y2);
    if (ctx.cull_face && area >= 0) return;  /* positive = back-facing after Y-flip */
    if (fabsf_(area) < 0.5f) return;

    /* Ensure positive area for barycentric math */
    if (area < 0) {
        vertex_t* tmp = v1; v1 = v2; v2 = tmp;
        x1 = v1->screen[0]; y1 = v1->screen[1];
        x2 = v2->screen[0]; y2 = v2->screen[1];
        area = -area;
    }
    float inv_area = 1.0f / area;

    int ix0 = (int)x0, iy0 = (int)y0;
    int ix1 = (int)x1, iy1 = (int)y1;
    int ix2 = (int)x2, iy2 = (int)y2;

    int minX = min3i(ix0, ix1, ix2);
    int maxX = max3i(ix0, ix1, ix2);
    int minY = min3i(iy0, iy1, iy2);
    int maxY = max3i(iy0, iy1, iy2);

    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX >= ctx.width) maxX = ctx.width - 1;
    if (maxY >= ctx.height) maxY = ctx.height - 1;

    for (int py = minY; py <= maxY; py++) {
        for (int px = minX; px <= maxX; px++) {
            float fx = px + 0.5f, fy = py + 0.5f;
            float w0 = edge(x1, y1, x2, y2, fx, fy);
            float w1 = edge(x2, y2, x0, y0, fx, fy);
            float w2 = edge(x0, y0, x1, y1, fx, fy);

            if (w0 < 0 || w1 < 0 || w2 < 0) continue;

            w0 *= inv_area; w1 *= inv_area; w2 *= inv_area;

            /* Interpolate depth */
            float z = w0 * v0->screen[2] + w1 * v1->screen[2] + w2 * v2->screen[2];

            int idx = py * ctx.width + px;

            /* Depth test */
            if (ctx.depth_test && z >= ctx.zbuf[idx]) continue;
            ctx.zbuf[idx] = z;

            /* Interpolate color (Gouraud shading) */
            float r = w0 * v0->shade[0] + w1 * v1->shade[0] + w2 * v2->shade[0];
            float g = w0 * v0->shade[1] + w1 * v1->shade[1] + w2 * v2->shade[1];
            float b = w0 * v0->shade[2] + w1 * v1->shade[2] + w2 * v2->shade[2];

            uint8_t ir = (uint8_t)(clampf(r, 0, 1) * 255);
            uint8_t ig = (uint8_t)(clampf(g, 0, 1) * 255);
            uint8_t ib = (uint8_t)(clampf(b, 0, 1) * 255);
            ctx.fb[idx] = ((uint32_t)ir << 16) | ((uint32_t)ig << 8) | ib;
        }
    }
}

static void rasterize_line(vertex_t* v0, vertex_t* v1) {
    int x0 = (int)v0->screen[0], y0 = (int)v0->screen[1];
    int x1 = (int)v1->screen[0], y1 = (int)v1->screen[1];
    uint8_t r = (uint8_t)(clampf(v0->shade[0], 0, 1) * 255);
    uint8_t g = (uint8_t)(clampf(v0->shade[1], 0, 1) * 255);
    uint8_t b = (uint8_t)(clampf(v0->shade[2], 0, 1) * 255);
    uint32_t col = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;

    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
    dx = dx > 0 ? dx : -dx;
    dy = dy > 0 ? dy : -dy;
    int err = dx - dy;
    while (1) {
        if (x0 >= 0 && x0 < ctx.width && y0 >= 0 && y0 < ctx.height)
            ctx.fb[y0 * ctx.width + x0] = col;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

/* ---- End / Flush ---- */
void glEnd(void) {
    /* Transform all verts */
    for (int i = 0; i < ctx.vert_count; i++)
        transform_vertex(&ctx.verts[i]);

    int mode = ctx.prim_mode;

    if (mode == GL_TRIANGLES) {
        for (int i = 0; i + 2 < ctx.vert_count; i += 3)
            rasterize_triangle(&ctx.verts[i], &ctx.verts[i+1], &ctx.verts[i+2]);
    }
    else if (mode == GL_QUADS) {
        for (int i = 0; i + 3 < ctx.vert_count; i += 4) {
            rasterize_triangle(&ctx.verts[i], &ctx.verts[i+1], &ctx.verts[i+2]);
            rasterize_triangle(&ctx.verts[i], &ctx.verts[i+2], &ctx.verts[i+3]);
        }
    }
    else if (mode == GL_TRIANGLE_FAN || mode == GL_POLYGON) {
        for (int i = 1; i + 1 < ctx.vert_count; i++)
            rasterize_triangle(&ctx.verts[0], &ctx.verts[i], &ctx.verts[i+1]);
    }
    else if (mode == GL_TRIANGLE_STRIP) {
        for (int i = 0; i + 2 < ctx.vert_count; i++) {
            if (i % 2 == 0)
                rasterize_triangle(&ctx.verts[i], &ctx.verts[i+1], &ctx.verts[i+2]);
            else
                rasterize_triangle(&ctx.verts[i+1], &ctx.verts[i], &ctx.verts[i+2]);
        }
    }
    else if (mode == GL_LINES) {
        for (int i = 0; i + 1 < ctx.vert_count; i += 2)
            rasterize_line(&ctx.verts[i], &ctx.verts[i+1]);
    }
    else if (mode == GL_LINE_STRIP) {
        for (int i = 0; i + 1 < ctx.vert_count; i++)
            rasterize_line(&ctx.verts[i], &ctx.verts[i+1]);
    }

    ctx.vert_count = 0;
}

void glSwapBuffers(void) {
    /* No-op — caller blits fb to screen */
}
