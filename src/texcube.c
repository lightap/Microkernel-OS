#include "userlib.h"

/*
 * texcube.c – ELF user-space program: rotating textured cube + benchmark
 *
 * FPS is calculated using sys_get_ticks() to measure actual time
 */

#define FB_W ELF_GUI_FB_W   // 320
#define FB_H ELF_GUI_FB_H   // 200
#define RGB(r,g,b) ((uint32_t)(((r)<<16)|((g)<<8)|(b)))

/* Texture */
#define TEX_SIZE 64
typedef struct {
    uint32_t data[TEX_SIZE * TEX_SIZE];
} texture_t;
static texture_t textures[6];

/* Z-buffer */
static float zbuffer[FB_W * FB_H];

/* Framebuffer */
static uint32_t* fb;

/* FPS tracking */
static uint32_t last_ticks = 0;
static uint32_t frame_count = 0;
static float current_fps = 0.0f;

/* ──────────────────────────────────────────────── */
/* Drawing helpers                                  */
/* ──────────────────────────────────────────────── */

static inline void putpx(int x, int y, uint32_t col) {
    if ((unsigned)x < FB_W && (unsigned)y < FB_H)
        fb[y * FB_W + x] = col;
}

static inline void putpx_depth(int x, int y, uint32_t col, float z) {
    if ((unsigned)x >= FB_W || (unsigned)y >= FB_H) return;
    int idx = y * FB_W + x;
    if (z < zbuffer[idx]) {
        zbuffer[idx] = z;
        fb[idx] = col;
    }
}

static uint32_t sample_texture(texture_t* tex, float u, float v) {
    u -= (int)u; if (u < 0) u += 1.0f;
    v -= (int)v; if (v < 0) v += 1.0f;

    float fu = u * (TEX_SIZE - 1);
    float fv = v * (TEX_SIZE - 1);
    int u0 = (int)fu, u1 = (u0 + 1) % TEX_SIZE;
    int v0 = (int)fv, v1 = (v0 + 1) % TEX_SIZE;

    float fracU = fu - u0;
    float fracV = fv - v0;

    uint32_t c00 = tex->data[v0 * TEX_SIZE + u0];
    uint32_t c10 = tex->data[v0 * TEX_SIZE + u1];
    uint32_t c01 = tex->data[v1 * TEX_SIZE + u0];
    uint32_t c11 = tex->data[v1 * TEX_SIZE + u1];

    int r = (int)((((c00>>16)&0xFF)*(1-fracU) + ((c10>>16)&0xFF)*fracU)*(1-fracV) +
                  ((((c01>>16)&0xFF)*(1-fracU) + ((c11>>16)&0xFF)*fracU))*fracV);
    int g = (int)((((c00>>8)&0xFF)*(1-fracU) + ((c10>>8)&0xFF)*fracU)*(1-fracV) +
                  ((((c01>>8)&0xFF)*(1-fracU) + ((c11>>8)&0xFF)*fracU))*fracV);
    int b = (int)((((c00&0xFF)*(1-fracU) + (c10&0xFF)*fracU)*(1-fracV) +
                  ((c01&0xFF)*(1-fracU) + (c11&0xFF)*fracU)*fracV));

    return RGB(r, g, b);
}

/* Procedural textures */
static void generate_checkerboard(texture_t* tex, uint32_t c1, uint32_t c2) {
    for (int y = 0; y < TEX_SIZE; y++)
        for (int x = 0; x < TEX_SIZE; x++)
            tex->data[y*TEX_SIZE + x] = (((x/8)+(y/8))&1) ? c1 : c2;
}

static void generate_gradient(texture_t* tex, uint32_t c1, uint32_t c2) {
    for (int y = 0; y < TEX_SIZE; y++)
        for (int x = 0; x < TEX_SIZE; x++) {
            float t = (float)y / TEX_SIZE;
            int r = ((c1>>16)&0xFF)*(1-t) + ((c2>>16)&0xFF)*t;
            int g = ((c1>>8)&0xFF)*(1-t) + ((c2>>8)&0xFF)*t;
            int b = (c1&0xFF)*(1-t) + (c2&0xFF)*t;
            tex->data[y*TEX_SIZE + x] = RGB(r,g,b);
        }
}

static void generate_circles(texture_t* tex, uint32_t c1, uint32_t c2) {
    float cx = TEX_SIZE/2.0f, cy = TEX_SIZE/2.0f;
    for (int y = 0; y < TEX_SIZE; y++)
        for (int x = 0; x < TEX_SIZE; x++) {
            float d = (x-cx)*(x-cx) + (y-cy)*(y-cy);
            tex->data[y*TEX_SIZE + x] = (((int)d/100)&1) ? c1 : c2;
        }
}

static void generate_stripes(texture_t* tex, uint32_t c1, uint32_t c2, int horizontal) {
    for (int y = 0; y < TEX_SIZE; y++)
        for (int x = 0; x < TEX_SIZE; x++) {
            int stripe = horizontal ? ((y/4)&1) : ((x/4)&1);
            tex->data[y*TEX_SIZE + x] = stripe ? c1 : c2;
        }
}

static void generate_dots(texture_t* tex, uint32_t c1, uint32_t c2) {
    for (int y = 0; y < TEX_SIZE; y++)
        for (int x = 0; x < TEX_SIZE; x++) {
            int dx = (x%16)-8, dy = (y%16)-8;
            tex->data[y*TEX_SIZE + x] = (dx*dx + dy*dy < 16) ? c1 : c2;
        }
}

static void generate_grid(texture_t* tex, uint32_t c1, uint32_t c2) {
    for (int y = 0; y < TEX_SIZE; y++)
        for (int x = 0; x < TEX_SIZE; x++)
            tex->data[y*TEX_SIZE + x] = ((x%16)<2 || (y%16)<2) ? c1 : c2;
}



/* 5×7 mini font – full 96 glyphs */
static const uint8_t mini_font[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14}, {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00}, {0x08,0x2A,0x1C,0x2A,0x08}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02}, {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00}, {0x00,0x08,0x14,0x22,0x41}, {0x14,0x14,0x14,0x14,0x14},
    {0x41,0x22,0x14,0x08,0x00}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x01,0x01},
    {0x3E,0x41,0x51,0x32}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F}, {0x7F,0x20,0x18,0x20,0x7F}, {0x63,0x14,0x08,0x14,0x63},
    {0x03,0x04,0x78,0x04,0x03}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x00,0x7F,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20}, {0x41,0x41,0x7F,0x00,0x00}, {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40}, {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x08,0x14,0x54,0x54,0x3C},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00},
    {0x00,0x7F,0x10,0x28,0x44}, {0x00,0x41,0x7F,0x41,0x00}, {0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, {0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C}, {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00}, {0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00}, {0x08,0x08,0x2A,0x1C,0x08}, {0x00,0x00,0x00,0x00,0x00}
};

/* Font drawing */
static void draw_char(int x, int y, char c, uint32_t col) {
    if ((unsigned char)c < 32 || (unsigned char)c > 127) return;
    const uint8_t* glyph = mini_font[c - 32];
    for (int gx = 0; gx < 5; gx++)
        for (int gy = 0; gy < 7; gy++)
            if (glyph[gx] & (1 << gy))
                putpx(x + gx, y + gy, col);
}

static void draw_str(int x, int y, const char* s, uint32_t col) {
    while (*s) {
        draw_char(x, y, *s++, col);
        x += 6;
    }
}

/* 3D math */
typedef struct { float x, y, z; } vec3_t;
typedef struct { float u, v; } vec2_t;

vec3_t rotate_x(vec3_t p, float a) {
    float c = u_cos(a), s = u_sin(a);
    return (vec3_t){ p.x, p.y*c - p.z*s, p.y*s + p.z*c };
}

vec3_t rotate_y(vec3_t p, float a) {
    float c = u_cos(a), s = u_sin(a);
    return (vec3_t){ p.x*c + p.z*s, p.y, -p.x*s + p.z*c };
}

vec3_t rotate_z(vec3_t p, float a) {
    float c = u_cos(a), s = u_sin(a);
    return (vec3_t){ p.x*c - p.y*s, p.x*s + p.y*c, p.z };
}

static void project(vec3_t p, int* sx, int* sy, float* depth) {
    float z = p.z + 60.0f;
    if (z < 0.1f) z = 0.1f;
    float scale = 100.0f / z;
    *sx = (int)(FB_W / 2 + p.x * scale);
    *sy = (int)(FB_H / 2 - p.y * scale);
    *depth = p.z;
}

/* Textured triangle rasterizer */
static void draw_textured_triangle(
    int x0, int y0, float z0, float u0, float v0,
    int x1, int y1, float z1, float u1, float v1,
    int x2, int y2, float z2, float u2, float v2,
    texture_t* tex)
{
    /* Sort vertices by y */
    if (y1 < y0) { int t; float tf;
        t=x0;x0=x1;x1=t; t=y0;y0=y1;y1=t;
        tf=z0;z0=z1;z1=tf; tf=u0;u0=u1;u1=tf; tf=v0;v0=v1;v1=tf; }
    if (y2 < y0) { int t; float tf;
        t=x0;x0=x2;x2=t; t=y0;y0=y2;y2=t;
        tf=z0;z0=z2;z2=tf; tf=u0;u0=u2;u2=tf; tf=v0;v0=v2;v2=tf; }
    if (y2 < y1) { int t; float tf;
        t=x1;x1=x2;x2=t; t=y1;y1=y2;y2=t;
        tf=z1;z1=z2;z2=tf; tf=u1;u1=u2;u2=tf; tf=v1;v1=v2;v2=tf; }

    if (y0 == y2) return;

    for (int y = y0; y <= y2; y++) {
        if (y < 0 || y >= FB_H) continue;

        float t_long = (float)(y - y0) / (y2 - y0);
        int xa = x0 + (int)((x2 - x0) * t_long);
        float za = z0 + (z2 - z0) * t_long;
        float ua = u0 + (u2 - u0) * t_long;
        float va = v0 + (v2 - v0) * t_long;

        int xb; float zb, ub, vb;
        if (y < y1) {
            float t = (y0 == y1) ? 0 : (float)(y - y0) / (y1 - y0);
            xb = x0 + (int)((x1 - x0) * t);
            zb = z0 + (z1 - z0) * t;
            ub = u0 + (u1 - u0) * t;
            vb = v0 + (v1 - v0) * t;
        } else {
            float t = (y1 == y2) ? 0 : (float)(y - y1) / (y2 - y1);
            xb = x1 + (int)((x2 - x1) * t);
            zb = z1 + (z2 - z1) * t;
            ub = u1 + (u2 - u1) * t;
            vb = v1 + (v2 - v1) * t;
        }

        if (xa > xb) {
            int tx = xa; xa = xb; xb = tx;
            float tz = za; za = zb; zb = tz;
            float tu = ua; ua = ub; ub = tu;
            float tv = va; va = vb; vb = tv;
        }

        int span = xb - xa;
        if (span <= 0) {
            putpx_depth(xa, y, sample_texture(tex, ua, va), za);
            continue;
        }

        for (int x = xa; x <= xb; x++) {
            if (x < 0 || x >= FB_W) continue;
            float t = (float)(x - xa) / span;
            float z = za + (zb - za) * t;
            float u = ua + (ub - ua) * t;
            float v = va + (vb - va) * t;
            putpx_depth(x, y, sample_texture(tex, u, v), z);
        }
    }
}

/* Main */
int main(void) {
    sys_debug_log("texcube benchmark starting\n");

    uint32_t fb_addr = sys_gui_win_open("TexCube Benchmark");
    if (!fb_addr) {
        sys_debug_log("Failed to open GUI window\n");
        return 1;
    }
    fb = (uint32_t*)fb_addr;

    /* Generate textures */
    generate_checkerboard(&textures[0], RGB(255,60,60),   RGB(180,30,30));
    generate_gradient(&textures[1],   RGB(60,60,255),     RGB(200,200,255));
    generate_circles(&textures[2],    RGB(60,255,60),     RGB(20,100,20));
    generate_stripes(&textures[3],    RGB(255,200,60),    RGB(180,140,20), 1);
    generate_dots(&textures[4],       RGB(255,60,255),    RGB(100,20,100));
    generate_grid(&textures[5],       RGB(60,255,255),    RGB(20,80,80));

    /* Cube vertices */
    vec3_t cube[8] = {
        {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
        {-1,-1,1},  {1,-1,1},  {1,1,1},  {-1,1,1}
    };

    /* Triangle indices */
    int indices[12][3] = {
        {0,1,2}, {0,2,3},     // front
        {5,4,7}, {5,7,6},     // back
        {4,0,3}, {4,3,7},     // left
        {1,5,6}, {1,6,2},     // right
        {3,2,6}, {3,6,7},     // top
        {4,5,1}, {4,1,0}      // bottom
    };

    /* Triangle structure */
    typedef struct {
        vec3_t v[3];
        vec2_t uv[3];
        int tex_id;
    } tri_t;

    tri_t tris[12];

    for (int i = 0; i < 12; i++) {
        tris[i].v[0] = cube[indices[i][0]];
        tris[i].v[1] = cube[indices[i][1]];
        tris[i].v[2] = cube[indices[i][2]];
        tris[i].uv[0] = (vec2_t){0,0};
        tris[i].uv[1] = (vec2_t){1,0};
        tris[i].uv[2] = (vec2_t){1,1};
        tris[i].tex_id = i / 2;
    }

    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    uint32_t frame = 0;

    sys_debug_log("Starting benchmark loop\n");

    /* Initialize timing */
    last_ticks = sys_get_ticks();
    frame_count = 0;

    while (1) {
        /* Clear */
        for (int i = 0; i < FB_W * FB_H; i++) {
            fb[i] = RGB(15,15,25);
            zbuffer[i] = 1000.0f;
        }

        /* Transform vertices */
        vec3_t transformed[8];
        for (int i = 0; i < 8; i++) {
            vec3_t v = cube[i];
            v = rotate_x(v, ax);
            v = rotate_y(v, ay);
            v = rotate_z(v, az);
            transformed[i] = v;
        }

        /* Draw triangles */
        for (int i = 0; i < 12; i++) {
            vec3_t v0 = transformed[indices[i][0]];
            vec3_t v1 = transformed[indices[i][1]];
            vec3_t v2 = transformed[indices[i][2]];

            /* Backface culling */
            float nx = (v1.y - v0.y)*(v2.z - v0.z) - (v1.z - v0.z)*(v2.y - v0.y);
            float ny = (v1.z - v0.z)*(v2.x - v0.x) - (v1.x - v0.x)*(v2.z - v0.z);
            float nz = (v1.x - v0.x)*(v2.y - v0.y) - (v1.y - v0.y)*(v2.x - v0.x);
            float vx = (v0.x + v1.x + v2.x)/3.0f;
            float vy = (v0.y + v1.y + v2.y)/3.0f;
            float vz = (v0.z + v1.z + v2.z)/3.0f + 60.0f;
            if (nx*vx + ny*vy + nz*vz <= 0) continue;

            /* Project */
            int px[3], py[3]; float pz[3];
            project(v0, &px[0], &py[0], &pz[0]);
            project(v1, &px[1], &py[1], &pz[1]);
            project(v2, &px[2], &py[2], &pz[2]);

            draw_textured_triangle(
                px[0], py[0], pz[0], tris[i].uv[0].u, tris[i].uv[0].v,
                px[1], py[1], pz[1], tris[i].uv[1].u, tris[i].uv[1].v,
                px[2], py[2], pz[2], tris[i].uv[2].u, tris[i].uv[2].v,
                &textures[tris[i].tex_id]
            );
        }

        ax += 0.012f;
        ay += 0.018f;
        az += 0.008f;

        frame++;
        frame_count++;

        /* Update FPS every second (1000 ticks) */
        uint32_t current_ticks = sys_get_ticks();
        uint32_t elapsed = current_ticks - last_ticks;
        
        if (elapsed >= 1000) {  /* 1000ms = 1 second */
            current_fps = (float)frame_count * 1000.0f / (float)elapsed;
            last_ticks = current_ticks;
            frame_count = 0;
        }

        /* HUD */
        draw_str(4,   4, "TexCube Benchmark", RGB(255,220,80));
        draw_str(4,  14, "Software 320x200",  RGB(200,180,60));
        draw_str(4,  FB_H-25, "Frame:",       RGB(180,180,220));

        char buf[32];
        utoa(frame, buf, 32);
        draw_str(50, FB_H-25, buf, RGB(220,220,255));

        draw_str(FB_W-160, 4, "FPS:", RGB(220,255,100));
        utoa((uint32_t)current_fps, buf, 32);
        draw_str(FB_W-120, 4, buf, RGB(220,255,100));

        sys_gui_present();

        /* Stop after ~2.5 minutes at 20 FPS (3000 frames) */
        if (frame > 3000) break;
    }

    /* Final report */
    char final[64];
    sys_debug_log("Benchmark finished. Total frames: ");
    utoa(frame, final, 32);
    sys_debug_log(final);
    sys_debug_log(", Final FPS: ");
    utoa((uint32_t)current_fps, final, 32);
    sys_debug_log(final);
    sys_debug_log("\n");

    while (1) sys_sleep(1000);

    return 0;
}