#include "userlib.h"

/*
 * hello.c - GPU-Accelerated 3D Spinning Pyramid
 *
 * Uses GPU3D syscall API to render via virgl/virtio-gpu:
 *   - Vertex data uploaded to GPU once
 *   - MVP matrix updated each frame (rotation animation)
 *   - GPU does all rasterization, depth testing, shading
 *
 * Falls back to CPU software rendering if GPU init fails.
 *
 * Run with: qemu-system-i386 -device virtio-gpu-gl-pci -display gtk,gl=on
 */

/* ---- Framebuffer (CPU fallback) ---- */
static uint32_t* fb;
#define FB_W  ELF_GUI_FB_W
#define FB_H  ELF_GUI_FB_H
#define RGB(r,g,b) ((uint32_t)(((r)<<16)|((g)<<8)|(b)))

static inline void putpx(int x, int y, uint32_t col) {
    if ((unsigned)x < FB_W && (unsigned)y < FB_H)
        fb[y * FB_W + x] = col;
}

/* ---- Tiny 5x7 font ---- */
static const uint8_t mini_font[96][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},
    {0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},
    {0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},
    {0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},
    {0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},
    {0x41,0x22,0x14,0x08,0x00},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},
    {0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},
    {0x7F,0x09,0x09,0x01,0x01},{0x3E,0x41,0x41,0x51,0x32},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x04,0x02,0x7F},
    {0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x7F,0x20,0x18,0x20,0x7F},
    {0x63,0x14,0x08,0x14,0x63},{0x03,0x04,0x78,0x04,0x03},
    {0x61,0x51,0x49,0x45,0x43},{0x00,0x00,0x7F,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20},{0x41,0x41,0x7F,0x00,0x00},
    {0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},
    {0x08,0x7E,0x09,0x01,0x02},{0x08,0x14,0x54,0x54,0x3C},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},
    {0x20,0x40,0x44,0x3D,0x00},{0x00,0x7F,0x10,0x28,0x44},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},
    {0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},
    {0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},
    {0x08,0x08,0x2A,0x1C,0x08},{0x00,0x00,0x00,0x00,0x00},
};

static void draw_char(int x, int y, char c, uint32_t col) {
    if ((unsigned char)c < 32 || (unsigned char)c > 127) return;
    const uint8_t* glyph = mini_font[c - 32];
    for (int gx = 0; gx < 5; gx++)
        for (int gy = 0; gy < 7; gy++)
            if (glyph[gx] & (1 << gy))
                putpx(x + gx, y + gy, col);
}

static void draw_str(int x, int y, const char* s, uint32_t col) {
    while (*s) { draw_char(x, y, *s++, col); x += 6; }
}

/* ---- 3D math ---- */
typedef struct { float x, y, z; } vec3_t;
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
                s += A->m[r*4+k] * B->m[k*4+c];
            R.m[r*4+c] = s;
        }
    *out = R;
}

static void mat4_perspective(mat4_t* M, float fovy, float aspect,
                              float zn, float zf) {
    float rad = fovy * 3.14159265f / 180.0f;
    float f = u_cos(rad / 2.0f) / u_sin(rad / 2.0f);
    memset(M, 0, sizeof(*M));
    M->m[0]  = f / aspect;
    M->m[5]  = f;
    M->m[10] = (zf + zn) / (zn - zf);
    M->m[11] = (2.0f * zf * zn) / (zn - zf);
    M->m[14] = -1.0f;
}

static void mat4_translate(mat4_t* M, float x, float y, float z) {
    mat4_identity(M);
    M->m[3] = x; M->m[7] = y; M->m[11] = z;
}

static void mat4_rotate_y(mat4_t* M, float a) {
    float c = u_cos(a), s = u_sin(a);
    mat4_identity(M);
    M->m[0] = c;  M->m[2] = s;
    M->m[8] = -s; M->m[10] = c;
}

static void mat4_rotate_x(mat4_t* M, float a) {
    float c = u_cos(a), s = u_sin(a);
    mat4_identity(M);
    M->m[5] = c;  M->m[6] = -s;
    M->m[9] = s;  M->m[10] = c;
}

/* ================================================================
 *  GPU PATH - Virgl hardware-accelerated rendering
 *  Vertex format: { x, y, z, r, g, b, a } = 7 floats per vertex
 * ================================================================ */

#define V(px,py,pz, cr,cg,cb)  px, py, pz, cr, cg, cb, 1.0f

static float gpu_pyramid_verts[] = {
    /* Front face (red) */
    V(-1.0f, -0.8f, -1.0f,  0.80f, 0.15f, 0.10f),
    V( 1.0f, -0.8f, -1.0f,  1.00f, 0.35f, 0.25f),
    V( 0.0f,  1.0f,  0.0f,  0.65f, 0.10f, 0.05f),
    /* Right face (green) */
    V( 1.0f, -0.8f, -1.0f,  0.15f, 0.80f, 0.10f),
    V( 1.0f, -0.8f,  1.0f,  0.25f, 1.00f, 0.35f),
    V( 0.0f,  1.0f,  0.0f,  0.10f, 0.65f, 0.05f),
    /* Back face (blue) */
    V( 1.0f, -0.8f,  1.0f,  0.10f, 0.15f, 0.80f),
    V(-1.0f, -0.8f,  1.0f,  0.25f, 0.35f, 1.00f),
    V( 0.0f,  1.0f,  0.0f,  0.05f, 0.10f, 0.65f),
    /* Left face (yellow) */
    V(-1.0f, -0.8f,  1.0f,  0.80f, 0.80f, 0.10f),
    V(-1.0f, -0.8f, -1.0f,  1.00f, 1.00f, 0.35f),
    V( 0.0f,  1.0f,  0.0f,  0.65f, 0.65f, 0.05f),
    /* Base triangle 1 */
    V(-1.0f, -0.8f, -1.0f,  0.35f, 0.20f, 0.15f),
    V( 1.0f, -0.8f,  1.0f,  0.45f, 0.25f, 0.20f),
    V( 1.0f, -0.8f, -1.0f,  0.35f, 0.20f, 0.15f),
    /* Base triangle 2 */
    V(-1.0f, -0.8f, -1.0f,  0.35f, 0.20f, 0.15f),
    V(-1.0f, -0.8f,  1.0f,  0.45f, 0.25f, 0.20f),
    V( 1.0f, -0.8f,  1.0f,  0.35f, 0.20f, 0.15f),
};

#define GPU_NUM_VERTS  18
#define GPU_NUM_FLOATS (GPU_NUM_VERTS * 7)

static void gpu_render_loop(void) {
    sys_debug_log("GPU3D: entering render loop\n");

    /* Upload static pyramid vertex data once */
    sys_gpu3d_upload(gpu_pyramid_verts, GPU_NUM_FLOATS);

    float angle_y = 0.0f;
    float angle_x = 0.35f;

    while (1) {
        /* Build MVP matrix: projection * translate * rotateX * rotateY */
        mat4_t proj, trans, rotY, rotX, tmp1, mv, mvp;
        mat4_perspective(&proj, 60.0f, 320.0f / 200.0f, 0.1f, 100.0f);
        mat4_translate(&trans, 0.0f, 0.0f, -5.0f);
        mat4_rotate_x(&rotX, angle_x);
        mat4_rotate_y(&rotY, angle_y);

        mat4_mul(&tmp1, &trans, &rotX);
        mat4_mul(&mv, &tmp1, &rotY);
        mat4_mul(&mvp, &proj, &mv);

        /* Clear color (dark blue) + depth */
//        sys_gpu3d_clear(GPU3D_CLEAR_COLOR | GPU3D_CLEAR_DEPTH, 0xFF0A0A14);
        sys_gpu3d_clear(GPU3D_CLEAR_COLOR | GPU3D_CLEAR_DEPTH, 0xFFFF0000);  /* bright red */

        /* Set MVP matrix as vertex shader constant buffer */
        sys_gpu3d_set_mvp(mvp.m);

        /* Draw pyramid triangles */
        sys_gpu3d_draw(GPU3D_TRIANGLES, 0, GPU_NUM_VERTS);

        /* Present framebuffer to display using VirGL */
        sys_gpu3d_present();

        /* Rotate for next frame */
        angle_y += 0.03f;

        /* ~60 FPS sleep */
        sys_sleep(16);
    }
}


/* ================================================================
 *  CPU FALLBACK PATH - Software rasterization
 * ================================================================ */

static vec3_t cpu_rotate_y(vec3_t p, float a) {
    float c = u_cos(a), s = u_sin(a);
    vec3_t r = { p.x*c + p.z*s, p.y, -p.x*s + p.z*c };
    return r;
}
static vec3_t cpu_rotate_x(vec3_t p, float a) {
    float c = u_cos(a), s = u_sin(a);
    vec3_t r = { p.x, p.y*c - p.z*s, p.y*s + p.z*c };
    return r;
}
static void cpu_project(vec3_t p, int* sx, int* sy) {
    float z = p.z + 4.0f;
    if (z < 0.1f) z = 0.1f;
    float scale = 200.0f / z;
    *sx = (int)(FB_W / 2 + p.x * scale);
    *sy = (int)(FB_H / 2 - p.y * scale);
}

static void draw_line(int x0, int y0, int x1, int y1, uint32_t col) {
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
    if (dx < 0) dx = -dx; if (dy < 0) dy = -dy;
    int err = dx - dy;
    for (;;) {
        putpx(x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

static void fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                           uint32_t c0, uint32_t c1, uint32_t c2) {
    int tx, ty; uint32_t tc;
    if (y0 > y1) { tx=x0;ty=y0;tc=c0; x0=x1;y0=y1;c0=c1; x1=tx;y1=ty;c1=tc; }
    if (y1 > y2) { tx=x1;ty=y1;tc=c1; x1=x2;y1=y2;c1=c2; x2=tx;y2=ty;c2=tc; }
    if (y0 > y1) { tx=x0;ty=y0;tc=c0; x0=x1;y0=y1;c0=c1; x1=tx;y1=ty;c1=tc; }
    if (y0 == y2) return;
    for (int y = y0; y <= y2; y++) {
        if (y < 0 || y >= FB_H) continue;
        int xa, xb; uint32_t ca, cb;
        float t_long = (float)(y - y0) / (float)(y2 - y0);
        xa = x0 + (int)((x2 - x0) * t_long);
        int ra = (int)(((c0>>16)&0xFF)*(1-t_long) + ((c2>>16)&0xFF)*t_long);
        int ga = (int)(((c0>>8)&0xFF)*(1-t_long) + ((c2>>8)&0xFF)*t_long);
        int ba = (int)((c0&0xFF)*(1-t_long) + (c2&0xFF)*t_long);
        ca = RGB(ra, ga, ba);
        if (y < y1) {
            if (y1 == y0) { xb = x0; cb = c0; }
            else { float t = (float)(y - y0) / (float)(y1 - y0);
                xb = x0 + (int)((x1 - x0) * t);
                cb = RGB((int)(((c0>>16)&0xFF)*(1-t)+((c1>>16)&0xFF)*t),
                         (int)(((c0>>8)&0xFF)*(1-t)+((c1>>8)&0xFF)*t),
                         (int)((c0&0xFF)*(1-t)+(c1&0xFF)*t)); }
        } else {
            if (y2 == y1) { xb = x1; cb = c1; }
            else { float t = (float)(y - y1) / (float)(y2 - y1);
                xb = x1 + (int)((x2 - x1) * t);
                cb = RGB((int)(((c1>>16)&0xFF)*(1-t)+((c2>>16)&0xFF)*t),
                         (int)(((c1>>8)&0xFF)*(1-t)+((c2>>8)&0xFF)*t),
                         (int)((c1&0xFF)*(1-t)+(c2&0xFF)*t)); }
        }
        if (xa > xb) { tx=xa; xa=xb; xb=tx; tc=ca; ca=cb; cb=tc; }
        int span = xb - xa;
        if (span <= 0) { putpx(xa, y, ca); continue; }
        for (int x = xa; x <= xb; x++) {
            float u = (float)(x - xa) / (float)span;
            putpx(x, y, RGB((int)(((ca>>16)&0xFF)*(1-u)+((cb>>16)&0xFF)*u),
                            (int)(((ca>>8)&0xFF)*(1-u)+((cb>>8)&0xFF)*u),
                            (int)((ca&0xFF)*(1-u)+(cb&0xFF)*u)));
        }
    }
}

static vec3_t cpu_verts[5] = {
    {-1.0f,-0.8f,-1.0f},{1.0f,-0.8f,-1.0f},{1.0f,-0.8f,1.0f},{-1.0f,-0.8f,1.0f},{0.0f,1.0f,0.0f},
};
static uint32_t fcolors[6][3] = {
    {RGB(200,50,50),RGB(255,100,80),RGB(180,40,40)},
    {RGB(50,200,50),RGB(80,255,100),RGB(40,180,40)},
    {RGB(50,50,200),RGB(80,100,255),RGB(40,40,180)},
    {RGB(200,200,50),RGB(255,255,100),RGB(180,180,40)},
    {RGB(100,50,50),RGB(120,70,70),RGB(80,40,40)},
    {RGB(100,50,50),RGB(120,70,70),RGB(80,40,40)},
};
static int fidx[6][3] = {{0,1,4},{1,2,4},{2,3,4},{3,0,4},{0,2,1},{0,3,2}};
typedef struct { int idx; float z; } fsort_t;

static void cpu_render_loop(void) {
    float ay = 0.0f, ax = 0.35f;
    uint32_t frame = 0;
    while (1) {
        for (int i = 0; i < FB_W*FB_H; i++) fb[i] = RGB(10,10,20);
        vec3_t tv[5]; int sx[5], sy[5];
        for (int i = 0; i < 5; i++) {
            tv[i] = cpu_rotate_y(cpu_verts[i], ay);
            tv[i] = cpu_rotate_x(tv[i], ax);
            cpu_project(tv[i], &sx[i], &sy[i]);
        }
        fsort_t faces[6];
        for (int i = 0; i < 6; i++) {
            faces[i].idx = i;
            faces[i].z = (tv[fidx[i][0]].z+tv[fidx[i][1]].z+tv[fidx[i][2]].z)/3.0f;
        }
        for (int i = 0; i < 5; i++)
            for (int j = i+1; j < 6; j++)
                if (faces[i].z < faces[j].z) { fsort_t t=faces[i]; faces[i]=faces[j]; faces[j]=t; }
        for (int f = 0; f < 6; f++) {
            int fi=faces[f].idx, i0=fidx[fi][0], i1=fidx[fi][1], i2=fidx[fi][2];
            fill_triangle(sx[i0],sy[i0],sx[i1],sy[i1],sx[i2],sy[i2],
                          fcolors[fi][0],fcolors[fi][1],fcolors[fi][2]);
            draw_line(sx[i0],sy[i0],sx[i1],sy[i1],RGB(255,255,255));
            draw_line(sx[i1],sy[i1],sx[i2],sy[i2],RGB(255,255,255));
            draw_line(sx[i2],sy[i2],sx[i0],sy[i0],RGB(255,255,255));
        }
        draw_str(4, 4, "CPU Software Render", RGB(255,100,100));
        draw_str(4, 14, "Spinning Pyramid", RGB(0,220,80));
        char fbuf[32] = "Frame: ";
        utoa(frame, fbuf+7, 10);
        draw_str(4, FB_H-10, fbuf, RGB(120,120,140));
        sys_gui_present();
        ay += 0.04f; frame++;
        sys_sleep(16);
    }
}



/* ================================================================
 *  MAIN - Try GPU, fall back to CPU
 * ================================================================ */
int main(void) {
    sys_debug_log("hello.elf starting...\n");

    uint32_t win = sys_gui_win_open("GPU Pyramid");
    if (!win) {
        sys_debug_log("Failed to open window\n");
        sys_exit(1);
    }

    sys_debug_log("hello.elf: attempting GPU3D init...\n");

    int32_t gpu_ok = sys_gpu3d_init(320, 200);

    if (gpu_ok == 0) {
        sys_debug_log("hello.elf: GPU3D ready! Hardware acceleration active.\n");

        gpu_render_loop();

        sys_exit(0);   // NEVER fall through
    }

    /* fallback */
    sys_debug_log("hello.elf: GPU3D unavailable, using CPU fallback\n");

    cpu_render_loop();

    return 0;
}