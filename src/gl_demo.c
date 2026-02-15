#include "gl_demo.h"
#include "minigl.h"
#include "bga.h"
#include "pci.h"
#include "paging.h"
#include "keyboard.h"
#include "timer.h"
#include "vga.h"
#include "heap.h"
#include "procfs.h"

/* BGA registers */
#define BGA_IO_INDEX  0x01CE
#define BGA_IO_DATA   0x01CF
#define BGA_REG_XRES  0x01
#define BGA_REG_YRES  0x02
#define BGA_REG_BPP   0x03
#define BGA_REG_EN    0x04
#define BGA_REG_VW    0x06
#define BGA_REG_VH    0x07
#define BGA_DIS       0x00
#define BGA_EN_FLAG   0x01
#define BGA_LFB       0x40
#define DEMO_FB_VIRT  0x21000000

#define MY_PI 3.14159265358979f

static uint16_t demo_w = 640, demo_h = 480;
static volatile uint32_t* hw_fb = NULL;
static uint32_t* sw_fb = NULL;

static bool demo_init_gfx(void) {
    outw(BGA_IO_INDEX, 0x00);
    uint16_t id = inw(BGA_IO_DATA);
    if (id < 0xB0C0 || id > 0xB0CF) return false;

    pci_device_t* dev = pci_find_device(0x1234, 0x1111);
    if (!dev) dev = pci_find_class(0x03, 0x00);
    if (!dev || !dev->bar[0]) return false;
    uint32_t fb_phys = dev->bar[0] & 0xFFFFFFF0;

    paging_map_range(DEMO_FB_VIRT, fb_phys, 4 * 1024 * 1024, PAGE_PRESENT | PAGE_WRITE);
    hw_fb = (volatile uint32_t*)DEMO_FB_VIRT;

    outw(BGA_IO_INDEX, BGA_REG_EN);   outw(BGA_IO_DATA, BGA_DIS);
    outw(BGA_IO_INDEX, BGA_REG_XRES); outw(BGA_IO_DATA, demo_w);
    outw(BGA_IO_INDEX, BGA_REG_YRES); outw(BGA_IO_DATA, demo_h);
    outw(BGA_IO_INDEX, BGA_REG_BPP);  outw(BGA_IO_DATA, 32);
    outw(BGA_IO_INDEX, BGA_REG_VW);   outw(BGA_IO_DATA, demo_w);
    outw(BGA_IO_INDEX, BGA_REG_VH);   outw(BGA_IO_DATA, demo_h);
    outw(BGA_IO_INDEX, BGA_REG_EN);   outw(BGA_IO_DATA, BGA_EN_FLAG | BGA_LFB);

    return true;
}

static void demo_blit(void) {
    uint32_t n = (uint32_t)demo_w * demo_h;
    for (uint32_t i = 0; i < n; i++) hw_fb[i] = sw_fb[i];
}

/* ---- Tiny 5x7 HUD font ---- */
static const uint8_t hfont[96][5] = {
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

static void hud_char(int x, int y, char c, uint32_t col) {
    if ((unsigned char)c < 32 || (unsigned char)c > 127) return;
    const uint8_t* glyph = hfont[c - 32];
    for (int gx = 0; gx < 5; gx++)
        for (int gy = 0; gy < 7; gy++)
            if (glyph[gx] & (1 << gy)) {
                int px = x + gx, py = y + gy;
                if (px >= 0 && px < demo_w && py >= 0 && py < demo_h)
                    sw_fb[py * demo_w + px] = col;
            }
}

static void hud_text(int x, int y, const char* s, uint32_t col) {
    while (*s) { hud_char(x, y, *s, col); x += 6; s++; }
}

/* ---- 3D Objects ---- */
void gl_draw_cube(float size) {
    float s = size / 2.0f;
    glNormal3f(0, 0, 1); glColor3f(1.0f, 0.2f, 0.2f);
    glVertex3f(-s,-s, s); glVertex3f( s,-s, s); glVertex3f( s, s, s); glVertex3f(-s, s, s);
    glNormal3f(0, 0,-1); glColor3f(0.2f, 1.0f, 0.2f);
    glVertex3f( s,-s,-s); glVertex3f(-s,-s,-s); glVertex3f(-s, s,-s); glVertex3f( s, s,-s);
    glNormal3f(0, 1, 0); glColor3f(0.2f, 0.2f, 1.0f);
    glVertex3f(-s, s, s); glVertex3f( s, s, s); glVertex3f( s, s,-s); glVertex3f(-s, s,-s);
    glNormal3f(0,-1, 0); glColor3f(1.0f, 1.0f, 0.2f);
    glVertex3f(-s,-s,-s); glVertex3f( s,-s,-s); glVertex3f( s,-s, s); glVertex3f(-s,-s, s);
    glNormal3f( 1, 0, 0); glColor3f(1.0f, 0.2f, 1.0f);
    glVertex3f( s,-s, s); glVertex3f( s,-s,-s); glVertex3f( s, s,-s); glVertex3f( s, s, s);
    glNormal3f(-1, 0, 0); glColor3f(0.2f, 1.0f, 1.0f);
    glVertex3f(-s,-s,-s); glVertex3f(-s,-s, s); glVertex3f(-s, s, s); glVertex3f(-s, s,-s);
}

void gl_draw_pyramid(float size) {
    float s = size / 2.0f, h = size * 0.8f;
    glNormal3f(0,-1, 0); glColor3f(0.8f, 0.6f, 0.2f);
    glVertex3f(-s,0,-s); glVertex3f( s,0,-s); glVertex3f( s,0, s); glVertex3f(-s,0, s);
    glNormal3f(0, 0.5f, 0.5f); glColor3f(1.0f, 0.3f, 0.1f);
    glVertex3f(-s,0, s); glVertex3f( s,0, s); glVertex3f( 0,h, 0); glVertex3f( 0,h, 0);
    glNormal3f(0.5f, 0.5f, 0); glColor3f(0.1f, 0.8f, 0.3f);
    glVertex3f( s,0, s); glVertex3f( s,0,-s); glVertex3f( 0,h, 0); glVertex3f( 0,h, 0);
    glNormal3f(0, 0.5f,-0.5f); glColor3f(0.3f, 0.3f, 1.0f);
    glVertex3f( s,0,-s); glVertex3f(-s,0,-s); glVertex3f( 0,h, 0); glVertex3f( 0,h, 0);
    glNormal3f(-0.5f, 0.5f, 0); glColor3f(0.9f, 0.9f, 0.1f);
    glVertex3f(-s,0,-s); glVertex3f(-s,0, s); glVertex3f( 0,h, 0); glVertex3f( 0,h, 0);
}

void gl_draw_torus(float R, float r, int rings, int sides) {
    for (int i = 0; i < rings; i++) {
        float t0 = 2.0f*MY_PI*i/rings, t1 = 2.0f*MY_PI*(i+1)/rings;
        float ct0=gl_cos(t0), st0=gl_sin(t0), ct1=gl_cos(t1), st1=gl_sin(t1);
        for (int j = 0; j < sides; j++) {
            float p0 = 2.0f*MY_PI*j/sides, p1 = 2.0f*MY_PI*(j+1)/sides;
            float cp0=gl_cos(p0), sp0=gl_sin(p0), cp1=gl_cos(p1), sp1=gl_sin(p1);
            glColor3f(0.5f+0.5f*gl_sin(t0*2), 0.5f+0.5f*gl_sin(t0*2+2.09f), 0.5f+0.5f*gl_sin(t0*2+4.19f));
            glNormal3f(cp0*ct0, sp0, cp0*st0); glVertex3f((R+r*cp0)*ct0, r*sp0, (R+r*cp0)*st0);
            glNormal3f(cp1*ct0, sp1, cp1*st0); glVertex3f((R+r*cp1)*ct0, r*sp1, (R+r*cp1)*st0);
            glNormal3f(cp1*ct1, sp1, cp1*st1); glVertex3f((R+r*cp1)*ct1, r*sp1, (R+r*cp1)*st1);
            glNormal3f(cp0*ct1, sp0, cp0*st1); glVertex3f((R+r*cp0)*ct1, r*sp0, (R+r*cp0)*st1);
        }
    }
}

void gl_draw_sphere(float radius, int slices, int stacks) {
    for (int i = 0; i < stacks; i++) {
        float la0 = MY_PI*(-0.5f+(float)i/stacks), la1 = MY_PI*(-0.5f+(float)(i+1)/stacks);
        float y0=gl_sin(la0), y1=gl_sin(la1), r0=gl_cos(la0), r1=gl_cos(la1);
        for (int j = 0; j < slices; j++) {
            float lo0 = 2.0f*MY_PI*j/slices, lo1 = 2.0f*MY_PI*(j+1)/slices;
            float x00=r0*gl_cos(lo0), z00=r0*gl_sin(lo0), x01=r0*gl_cos(lo1), z01=r0*gl_sin(lo1);
            float x10=r1*gl_cos(lo0), z10=r1*gl_sin(lo0), x11=r1*gl_cos(lo1), z11=r1*gl_sin(lo1);
            float hue = 0.5f+0.5f*gl_sin(la0*3+lo0);
            glColor3f(0.3f+0.7f*hue, 0.3f+0.4f*(1-hue), 0.6f);
            glNormal3f(x00,y0,z00); glVertex3f(x00*radius, y0*radius, z00*radius);
            glNormal3f(x01,y0,z01); glVertex3f(x01*radius, y0*radius, z01*radius);
            glNormal3f(x11,y1,z11); glVertex3f(x11*radius, y1*radius, z11*radius);
            glNormal3f(x10,y1,z10); glVertex3f(x10*radius, y1*radius, z10*radius);
        }
    }
}

void gl_draw_floor(void) {
    glDisable(GL_LIGHTING);
    glBegin(GL_LINES);
    glColor3f(0.2f, 0.2f, 0.25f);
    for (int i = -4; i <= 4; i++) {
        float p = (float)i;
        glVertex3f(p, -1.5f, -4); glVertex3f(p, -1.5f, 4);
        glVertex3f(-4, -1.5f, p); glVertex3f(4, -1.5f, p);
    }
    glEnd();
    glEnable(GL_LIGHTING);
}

void gl_draw_axes(void) {
    glDisable(GL_LIGHTING);
    glBegin(GL_LINES);
    glColor3f(1,0,0); glVertex3f(0,0,0); glVertex3f(2,0,0);
    glColor3f(0,1,0); glVertex3f(0,0,0); glVertex3f(0,2,0);
    glColor3f(0,0,1); glVertex3f(0,0,0); glVertex3f(0,0,2);
    glEnd();
    glEnable(GL_LIGHTING);
}

/* ---- Main demo loop ---- */
void gl_demo_start(void) {
    if (!demo_init_gfx()) {
        kprintf("GL demo: no BGA display. Need QEMU with VGA.\n");
        return;
    }

    uint32_t fb_size = (uint32_t)demo_w * demo_h * sizeof(uint32_t);
    sw_fb = (uint32_t*)kmalloc(fb_size);
    if (!sw_fb) { kprintf("GL demo: out of memory\n"); return; }

    glInit(sw_fb, demo_w, demo_h);
    glViewport(0, 0, demo_w, demo_h);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_CULL_FACE);

    float lpos[] = {2,3,4,0}, lamb[] = {0.15f,0.15f,0.18f,1}, ldif[] = {0.9f,0.85f,0.8f,1};
    glLightfv(GL_LIGHT0, GL_POSITION, lpos);
    glLightfv(GL_LIGHT0, GL_AMBIENT,  lamb);
    glLightfv(GL_LIGHT0, GL_DIFFUSE,  ldif);

    float ax = 25, ay = 0, dist = 5;
    int scene = 0;
    const char* names[] = {"Cube","Torus","Pyramid","Sphere"};
    bool wire = false, autorot = true, axes = true, floor_on = true, running = true;
    uint32_t frame = 0, fps_t = timer_get_ticks();
    int fps = 0, fpsc = 0;

    while (running) {
        char ch = keyboard_trychar();
        if (ch) {
            if (ch == 27) running = false;
            else if (ch == ' ') scene = (scene+1) % 4;
            else if (ch == 'w') wire = !wire;
            else if (ch == 'r') autorot = !autorot;
            else if (ch == 'a') axes = !axes;
            else if (ch == 'f') floor_on = !floor_on;
            else if (ch == '+' || ch == '=') { dist -= 0.5f; if (dist < 2) dist = 2; }
            else if (ch == '-') { dist += 0.5f; if (dist > 20) dist = 20; }
            else if (ch == 'h') ay -= 8;
            else if (ch == 'l') ay += 8;
            else if (ch == 'k') ax -= 5;
            else if (ch == 'j') ax += 5;
        }
        if (autorot) ay += 1.0f;

        glClearColor(0.04f, 0.04f, 0.1f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        gluPerspective(60, (float)demo_w / demo_h, 0.1f, 100);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(0, 0, -dist);
        glRotatef(ax, 1, 0, 0);
        glRotatef(ay, 0, 1, 0);

        if (floor_on) gl_draw_floor();
        if (axes) gl_draw_axes();

        if (wire) { glDisable(GL_LIGHTING); glPolygonMode(GL_LINE); }
        else      { glEnable(GL_LIGHTING);  glPolygonMode(GL_FILL); }

        glBegin(GL_QUADS);
        switch (scene) {
            case 0: gl_draw_cube(1.8f); break;
            case 1: gl_draw_torus(1.0f, 0.4f, 16, 12); break;
            case 2: gl_draw_pyramid(2.0f); break;
            case 3: gl_draw_sphere(1.2f, 16, 12); break;
        }
        glEnd();

        /* HUD */
        hud_text(6, 4, "MiniGL - Software OpenGL 1.1", 0x00DD44);
        char buf[80];
        ksnprintf(buf, 80, "Object: %s  FPS: %d  %dx%d", names[scene], fps, demo_w, demo_h);
        hud_text(6, 14, buf, 0x00AA33);
        hud_text(6, demo_h - 10, "ESC:quit SPC:next +/-:zoom HJKL:look R:rotate W:wire A:axes F:floor", 0x555555);

        demo_blit();

        fpsc++;
        uint32_t now = timer_get_ticks();
        if (now - fps_t >= 100) { fps = fpsc; fpsc = 0; fps_t = now; }
        frame++;

        uint32_t t = timer_get_ticks();
        while (timer_get_ticks() < t + 2) hlt();
    }

    glClose();
    kfree(sw_fb); sw_fb = NULL;

    outw(BGA_IO_INDEX, BGA_REG_EN);
    outw(BGA_IO_DATA, BGA_DIS);
    outb(0x3C2, 0x67);
    outb(0x3D4, 0x11); outb(0x3D5, inb(0x3D5) & 0x7F);
    terminal_init();
    terminal_clear();
    kprintf("GL demo exited. %u frames rendered.\n", frame);
}
