#include "gui.h"
#include "mouse.h"
#include "keyboard.h"
#include "timer.h"
#include "vga.h"
#include "ramfs.h"
#include "pci.h"
#include "paging.h"
#include "minifont.h"
#include "image.h"
#include "procfs.h"
#include "heap.h"
#include "fat16.h"
#include "ntfs.h"
#include "serial.h"
#include "login.h"
#include "minigl.h"
#include "gl_demo.h"
#include "elf.h"
#include "task.h"
#include "pmm.h"
#include "rtc.h"
#include "virtio_gpu.h"
#include "virgl.h" 

/* ====== CONSTANTS ====== */
static uint16_t GFX_W = 1920;
static uint16_t GFX_H = 1080;
#define FONT_W 5
#define FONT_H 7
#define TASKBAR_H  30
#define TITLEBAR_H 25
#define MAX_WINDOWS 8
#define MAX_WIN_TITLE 40

/* ====== BGA (Bochs VBE) REGISTERS ====== */
#define BGA_IOPORT_INDEX 0x01CE
#define BGA_IOPORT_DATA  0x01CF
#define BGA_REG_ID       0x00
#define BGA_REG_XRES     0x01
#define BGA_REG_YRES     0x02
#define BGA_REG_BPP      0x03
#define BGA_REG_ENABLE   0x04
#define BGA_REG_VWIDTH   0x06
#define BGA_REG_VHEIGHT  0x07
#define BGA_DISABLED     0x00
#define BGA_ENABLED      0x01
#define BGA_LFB_ENABLED  0x40

#define FB_VIRT_BASE 0x20000000

/* ====== RGB COLOR HELPERS ====== */
#define RGB(r,g,b)  ((uint32_t)(((r)<<16)|((g)<<8)|(b)))
#define RGBA(r,g,b,a) ((uint32_t)(((a)<<24)|((r)<<16)|((g)<<8)|(b)))
#define R(c) (((c)>>16)&0xFF)
#define G(c) (((c)>>8)&0xFF)
#define B(c) ((c)&0xFF)

static inline uint32_t rgb_lerp(uint32_t c0, uint32_t c1, int t) {
    int r = R(c0) + (((int)R(c1) - (int)R(c0)) * t) / 255;
    int g = G(c0) + (((int)G(c1) - (int)G(c0)) * t) / 255;
    int b = B(c0) + (((int)B(c1) - (int)B(c0)) * t) / 255;
    return RGB(r, g, b);
}

static inline uint32_t rgb_darken(uint32_t c, int factor) {
    return RGB((R(c)*factor)/255, (G(c)*factor)/255, (B(c)*factor)/255);
}

static inline uint32_t rgb_lighten(uint32_t c, int amount) {
    int r = R(c) + amount; if (r > 255) r = 255;
    int g = G(c) + amount; if (g > 255) g = 255;
    int b = B(c) + amount; if (b > 255) b = 255;
    return RGB(r, g, b);
}

/* Alpha blend: paints 'fg' over 'bg' with alpha 0-255 */
static inline uint32_t alpha_blend(uint32_t bg, uint32_t fg, int alpha) {
    int inv = 255 - alpha;
    int r = (R(fg) * alpha + R(bg) * inv) / 255;
    int g = (G(fg) * alpha + G(bg) * inv) / 255;
    int b = (B(fg) * alpha + B(bg) * inv) / 255;
    return RGB(r, g, b);
}

/* ================================================================ */
/*              WINDOWS XP "LUNA BLUE" COLOR PALETTE                */
/* ================================================================ */

/* --- Desktop --- */
#define COL_DESKTOP_TOP    RGB(0, 78, 152)    /* XP Bliss sky top */
#define COL_DESKTOP_BOT    RGB(55, 145, 220)  /* XP Bliss sky bottom */

/* --- Taskbar (Luna Blue gradient) --- */
#define COL_TASKBAR_TOP    RGB(21, 91, 189)   /* Top edge highlight */
#define COL_TASKBAR_MID    RGB(34, 102, 204)  /* Main body */
#define COL_TASKBAR_BOT    RGB(12, 58, 140)   /* Bottom edge */
#define COL_TASKBAR_SHINE  RGB(60, 127, 220)  /* Gloss shine line */

/* --- Start Button (XP green pill) --- */
#define COL_START_TOP      RGB(74, 175, 74)   /* Green top */
#define COL_START_MID      RGB(44, 135, 44)   /* Green middle */
#define COL_START_BOT      RGB(24, 100, 24)   /* Green bottom */
#define COL_START_SHINE    RGB(120, 210, 120) /* Green shine */
#define COL_START_HOV_TOP  RGB(90, 200, 90)   /* Hover green top */
#define COL_START_HOV_BOT  RGB(34, 120, 34)   /* Hover green bottom */
#define COL_START_TEXT     RGB(255, 255, 255)

/* --- Title bar (active window - XP blue gradient) --- */
#define COL_TITLE_T1       RGB(0, 88, 238)    /* Top of titlebar */
#define COL_TITLE_T2       RGB(4, 100, 206)   /* Upper-mid */
#define COL_TITLE_T3       RGB(18, 62, 172)   /* Lower-mid */
#define COL_TITLE_T4       RGB(22, 52, 146)   /* Bottom of titlebar */
#define COL_TITLE_SHINE    RGB(70, 140, 255)  /* Top shine highlight */

/* --- Title bar (inactive window - XP gray-blue) --- */
#define COL_ITITLE_T1      RGB(118, 151, 210) /* Inactive top */
#define COL_ITITLE_T2      RGB(126, 156, 210) /* Inactive mid */
#define COL_ITITLE_T3      RGB(108, 136, 186) /* Inactive lower */
#define COL_ITITLE_T4      RGB(94, 122, 170)  /* Inactive bottom */

/* --- Window body --- */
#define COL_WINBODY        RGB(236, 233, 216) /* XP window body (ecru/cream) */
#define COL_WINCLIENT      RGB(255, 255, 255) /* Client area (white) */
#define COL_WINBORDER_OUT  RGB(0, 60, 165)    /* Outer border (blue) active */
#define COL_WINBORDER_IN   RGB(127, 157, 185) /* Inner border */
#define COL_IBORDER_OUT    RGB(100, 126, 170) /* Inactive outer border */

/* --- Title text --- */
#define COL_TITLETXT       RGB(255, 255, 255)
#define COL_TITLETXT_SHD   RGB(0, 0, 90)
#define COL_ITITLETXT      RGB(216, 228, 248)

/* --- Caption buttons (XP style red/blue/blue) --- */
#define COL_CLOSE_TOP      RGB(210, 72, 56)   /* Close button top */
#define COL_CLOSE_BOT      RGB(164, 38, 28)   /* Close button bottom */
#define COL_CLOSE_HOV_TOP  RGB(242, 110, 90)  /* Close hover top */
#define COL_CLOSE_HOV_BOT  RGB(200, 60, 44)   /* Close hover bottom */
#define COL_CLOSE_SHINE    RGB(240, 150, 140) /* Close shine */
#define COL_CAPBTN_TOP     RGB(60, 107, 192)  /* Min/Max button top */
#define COL_CAPBTN_BOT     RGB(36, 67, 150)   /* Min/Max button bottom */
#define COL_CAPBTN_HOV_TOP RGB(90, 140, 220)

/* --- Standard XP-era controls --- */
#define COL_BTNFACE        RGB(236, 233, 216) /* Button face */
#define COL_BTNHI          RGB(255, 255, 255) /* Button highlight */
#define COL_BTNSHADOW      RGB(172, 168, 153) /* Button shadow */
#define COL_DKSHADOW       RGB(113, 111, 100) /* Dark shadow */
#define COL_BTNLIGHT       RGB(241, 239, 226) /* Light edge */

/* --- Generic colors --- */
#define COL_BLACK          RGB(0, 0, 0)
#define COL_WHITE          RGB(255, 255, 255)
#define COL_TEXT           RGB(0, 0, 0)
#define COL_GRAY           RGB(128, 128, 128)
#define COL_BORDER         RGB(0, 0, 0)

/* --- Start Menu (XP two-panel) --- */
#define COL_MENUBG         RGB(255, 255, 255)
#define COL_MENUHI         RGB(49, 106, 197)  /* XP selection blue */
#define COL_MENUHITEXT     RGB(255, 255, 255)
#define COL_MENUSEP        RGB(200, 200, 200)
#define COL_MENUPANEL_TOP  RGB(21, 66, 139)   /* Top banner */
#define COL_MENUPANEL_BOT  RGB(72, 112, 180)  /* Bottom banner gradient */
#define COL_MENUBOTTOM     RGB(214, 211, 200) /* Bottom panel */
#define COL_MENUBOTTOM_HI  RGB(49, 106, 197)

/* --- App-specific --- */
#define COL_CALCBTN        RGB(236, 233, 216)
#define COL_CALCDISP       RGB(224, 237, 219) /* Slightly green calc display */
#define COL_DIRBLUE        RGB(0, 0, 180)
#define COL_SCROLLBG       RGB(228, 225, 209)

/* --- Shadows --- */
#define COL_SHADOW         RGB(0, 0, 0)       /* Used with alpha blend */
#define COL_SHADOW_ALPHA   80                  /* Shadow transparency */

/* ====== FRAMEBUFFER ====== */
static uint32_t* backbuf = NULL;
static volatile uint32_t* framebuffer = NULL;
static uint32_t fb_phys = 0;
static bool using_virtio_gpu = false;  /* true when virtio-gpu is the backend */

/* ====== FONT DATA (for text mode restore) ====== */
static uint8_t font_data[128][16];

static void read_vga_font(void) {
    outb(0x3C4, 2); uint8_t seq2 = inb(0x3C5);
    outb(0x3C4, 4); uint8_t seq4 = inb(0x3C5);
    outb(0x3CE, 4); uint8_t gc4  = inb(0x3CF);
    outb(0x3CE, 5); uint8_t gc5  = inb(0x3CF);
    outb(0x3CE, 6); uint8_t gc6  = inb(0x3CF);
    outb(0x3C4, 2); outb(0x3C5, 0x04);
    outb(0x3C4, 4); outb(0x3C5, 0x06);
    outb(0x3CE, 4); outb(0x3CF, 0x02);
    outb(0x3CE, 5); outb(0x3CF, 0x00);
    outb(0x3CE, 6); outb(0x3CF, 0x00);
    volatile uint8_t* vmem = (volatile uint8_t*)0xA0000;
    for (int c = 0; c < 128; c++)
        for (int row = 0; row < 16; row++)
            font_data[c][row] = vmem[c * 32 + row];
    outb(0x3C4, 2); outb(0x3C5, seq2);
    outb(0x3C4, 4); outb(0x3C5, seq4);
    outb(0x3CE, 4); outb(0x3CF, gc4);
    outb(0x3CE, 5); outb(0x3CF, gc5);
    outb(0x3CE, 6); outb(0x3CF, gc6);
}

/* ====== BGA DRIVER ====== */
static void bga_write(uint16_t reg, uint16_t val) {
    outw(BGA_IOPORT_INDEX, reg);
    outw(BGA_IOPORT_DATA, val);
}
static uint16_t bga_read(uint16_t reg) {
    outw(BGA_IOPORT_INDEX, reg);
    return inw(BGA_IOPORT_DATA);
}
static bool bga_detect(void) {
    uint16_t id = bga_read(BGA_REG_ID);
    return (id >= 0xB0C0 && id <= 0xB0CF);
}
static bool bga_init_mode(uint16_t w, uint16_t h, uint16_t bpp) {
    pci_device_t* dev = pci_find_device(0x1234, 0x1111);
    if (!dev) dev = pci_find_class(0x03, 0x00);
    if (!dev || !dev->bar[0]) return false;
    fb_phys = dev->bar[0] & 0xFFFFFFF0;
    paging_map_range(FB_VIRT_BASE, fb_phys, 8 * 1024 * 1024, PAGE_PRESENT | PAGE_WRITE);
    framebuffer = (volatile uint32_t*)FB_VIRT_BASE;
    bga_write(BGA_REG_ENABLE, BGA_DISABLED);
    bga_write(BGA_REG_XRES, w);
    bga_write(BGA_REG_YRES, h);
    bga_write(BGA_REG_BPP, bpp);
    bga_write(BGA_REG_VWIDTH, w);
    bga_write(BGA_REG_VHEIGHT, h);
    bga_write(BGA_REG_ENABLE, BGA_ENABLED | BGA_LFB_ENABLED);
    return true;
}
static void bga_disable(void) {
    bga_write(BGA_REG_ENABLE, BGA_DISABLED);
}

/* ====== BLIT ====== */
static void blit(void) {
  
        /* virtio FB is regular RAM (identity-mapped) — memcpy is safe & fast */
        uint32_t* fb = virtio_gpu_get_fb();
        memcpy(fb, backbuf, GFX_W * GFX_H * sizeof(uint32_t));
        virtio_gpu_flush_all();
    
}

/* ====== DRAWING PRIMITIVES ====== */

static inline void putpixel(int x, int y, uint32_t col) {
    if ((unsigned)x < GFX_W && (unsigned)y < GFX_H)
        backbuf[y * GFX_W + x] = col;
}

/* Put pixel with alpha blending against existing backbuffer content */
static inline void putpixel_alpha(int x, int y, uint32_t col, int alpha) {
    if ((unsigned)x < GFX_W && (unsigned)y < GFX_H) {
        uint32_t bg = backbuf[y * GFX_W + x];
        backbuf[y * GFX_W + x] = alpha_blend(bg, col, alpha);
    }
}

static void fill_rect(int x, int y, int w, int h, uint32_t col) {
    for (int j = y; j < y + h; j++) {
        if ((unsigned)j >= GFX_H) continue;
        int off = j * GFX_W;
        int x0 = x < 0 ? 0 : x;
        int x1 = (x + w) > GFX_W ? GFX_W : (x + w);
        for (int i = x0; i < x1; i++)
            backbuf[off + i] = col;
    }
}

/* Fill rect with alpha blending */
static void fill_rect_alpha(int x, int y, int w, int h, uint32_t col, int alpha) {
    for (int j = y; j < y + h; j++) {
        if ((unsigned)j >= GFX_H) continue;
        int off = j * GFX_W;
        int x0 = x < 0 ? 0 : x;
        int x1 = (x + w) > GFX_W ? GFX_W : (x + w);
        for (int i = x0; i < x1; i++)
            backbuf[off + i] = alpha_blend(backbuf[off + i], col, alpha);
    }
}

static void draw_hline(int x, int y, int w, uint32_t col) {
    for (int i = x; i < x + w; i++) putpixel(i, y, col);
}
static void draw_vline(int x, int y, int h, uint32_t col) {
    for (int j = y; j < y + h; j++) putpixel(x, j, col);
}
static void draw_rect(int x, int y, int w, int h, uint32_t col) {
    draw_hline(x, y, w, col); draw_hline(x, y+h-1, w, col);
    draw_vline(x, y, h, col); draw_vline(x+w-1, y, h, col);
}

/* Horizontal gradient */
static void fill_gradient_h(int x, int y, int w, int h, uint32_t c0, uint32_t c1) {
    for (int j = y; j < y + h; j++) {
        if ((unsigned)j >= GFX_H) continue;
        int off = j * GFX_W;
        for (int i = 0; i < w; i++) {
            int xi = x + i;
            if ((unsigned)xi >= GFX_W) continue;
            int t = (i * 255) / (w > 1 ? w - 1 : 1);
            backbuf[off + xi] = rgb_lerp(c0, c1, t);
        }
    }
}

/* Vertical gradient */
static void fill_gradient_v(int x, int y, int w, int h, uint32_t c0, uint32_t c1) {
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if ((unsigned)yy >= GFX_H) continue;
        int off = yy * GFX_W;
        int t = (j * 255) / (h > 1 ? h - 1 : 1);
        uint32_t col = rgb_lerp(c0, c1, t);
        int x0 = x < 0 ? 0 : x;
        int x1 = (x + w) > GFX_W ? GFX_W : (x + w);
        for (int i = x0; i < x1; i++)
            backbuf[off + i] = col;
    }
}

/* Multi-stop vertical gradient (4 stops evenly spaced) */
static void fill_gradient_v4(int x, int y, int w, int h,
                              uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3) {
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if ((unsigned)yy >= GFX_H) continue;
        int off = yy * GFX_W;
        /* 4 stops: 0..h/3, h/3..2h/3, 2h/3..h */
        uint32_t col;
        int seg = h / 3;
        if (seg < 1) seg = 1;
        if (j < seg) {
            int t = (j * 255) / seg;
            col = rgb_lerp(c0, c1, t);
        } else if (j < seg * 2) {
            int t = ((j - seg) * 255) / seg;
            col = rgb_lerp(c1, c2, t);
        } else {
            int t = ((j - seg * 2) * 255) / (h - seg * 2);
            if (t > 255) t = 255;
            col = rgb_lerp(c2, c3, t);
        }
        int x0 = x < 0 ? 0 : x;
        int x1 = (x + w) > GFX_W ? GFX_W : (x + w);
        for (int i = x0; i < x1; i++)
            backbuf[off + i] = col;
    }
}

/* XP-style raised button with rounded feel */
static void draw_xp_button(int x, int y, int w, int h, uint32_t face, bool pressed) {
    if (pressed) {
        fill_rect(x, y, w, h, rgb_darken(face, 220));
        draw_rect(x, y, w, h, COL_DKSHADOW);
        draw_hline(x+1, y+1, w-2, COL_BTNSHADOW);
        draw_vline(x+1, y+1, h-2, COL_BTNSHADOW);
    } else {
        fill_rect(x, y, w, h, face);
        /* Top/left highlight */
        draw_hline(x, y, w, COL_BTNHI);
        draw_vline(x, y, h, COL_BTNHI);
        draw_hline(x+1, y+1, w-3, COL_BTNLIGHT);
        draw_vline(x+1, y+1, h-3, COL_BTNLIGHT);
        /* Bottom/right shadow */
        draw_hline(x, y+h-1, w, COL_DKSHADOW);
        draw_vline(x+w-1, y, h, COL_DKSHADOW);
        draw_hline(x+1, y+h-2, w-2, COL_BTNSHADOW);
        draw_vline(x+w-2, y+1, h-2, COL_BTNSHADOW);
    }
}

static void draw_sunken(int x, int y, int w, int h, uint32_t face) {
    fill_rect(x, y, w, h, face);
    draw_hline(x, y, w, COL_BTNSHADOW);
    draw_vline(x, y, h, COL_BTNSHADOW);
    draw_hline(x+1, y+1, w-2, COL_DKSHADOW);
    draw_vline(x+1, y+1, h-2, COL_DKSHADOW);
    draw_hline(x+1, y+h-1, w-1, COL_BTNHI);
    draw_vline(x+w-1, y+1, h-1, COL_BTNHI);
    draw_hline(x+2, y+h-2, w-3, COL_BTNLIGHT);
    draw_vline(x+w-2, y+2, h-3, COL_BTNLIGHT);
}

/* XP-style inset edit field (white area with 3D border) */
static void draw_xp_editbox(int x, int y, int w, int h) {
    fill_rect(x, y, w, h, COL_WHITE);
    /* Outer shadow (top-left) */
    draw_hline(x, y, w, COL_BTNSHADOW);
    draw_vline(x, y, h, COL_BTNSHADOW);
    /* Inner shadow */
    draw_hline(x+1, y+1, w-2, COL_DKSHADOW);
    draw_vline(x+1, y+1, h-2, COL_DKSHADOW);
    /* Outer highlight (bottom-right) */
    draw_hline(x, y+h-1, w, COL_BTNHI);
    draw_vline(x+w-1, y, h, COL_BTNHI);
    /* Inner highlight */
    draw_hline(x+1, y+h-2, w-2, COL_BTNLIGHT);
    draw_vline(x+w-2, y+1, h-2, COL_BTNLIGHT);
}

/* ====== TEXT ====== */
static void draw_char(int x, int y, char c, uint32_t col) {
    uint8_t uc = (uint8_t)c;
    if (uc < 32 || uc > 126) return;
    const uint8_t* glyph = minifont_data[uc - 32];
    for (int row = 0; row < MFONT_H; row++) {
        uint8_t bits = glyph[row];
        for (int bit = 0; bit < MFONT_W; bit++) {
            if (bits & (0x80 >> bit))
                putpixel(x + bit, y + row, col);
        }
    }
}

static void draw_char_shadow(int x, int y, char c, uint32_t col, uint32_t shd) {
    draw_char(x + 1, y + 1, c, shd);
    draw_char(x, y, c, col);
}

static void draw_text(int x, int y, const char* str, uint32_t col) {
    while (*str) { draw_char(x, y, *str, col); x += FONT_W; str++; }
}

static void draw_text_shadow(int x, int y, const char* str, uint32_t col, uint32_t shd) {
    const char* s = str;
    int sx = x;
    while (*s) { draw_char(sx + 1, y + 1, *s, shd); sx += FONT_W; s++; }
    while (*str) { draw_char(x, y, *str, col); x += FONT_W; str++; }
}

static void draw_text_clipped(int x, int y, const char* str, uint32_t col, int max_w) {
    int cx = 0;
    while (*str && cx + FONT_W <= max_w) {
        draw_char(x + cx, y, *str, col);
        cx += FONT_W; str++;
    }
}

/* Centered text in a rectangle */
static void draw_text_centered(int x, int y, int w, int h, const char* str, uint32_t col) {
    int tw = (int)strlen(str) * FONT_W;
    int tx = x + (w - tw) / 2;
    int ty = y + (h - FONT_H) / 2;
    draw_text(tx, ty, str, col);
}

static int text_width(const char* str) { return (int)strlen(str) * FONT_W; }

/* Bold text (draw twice offset by 1px) */
static void draw_text_bold(int x, int y, const char* str, uint32_t col) {
    draw_text(x, y, str, col);
    draw_text(x + 1, y, str, col);
}

static void draw_text_bold_shadow(int x, int y, const char* str, uint32_t col, uint32_t shd) {
    draw_text(x + 1, y + 1, str, shd);
    draw_text(x + 2, y + 1, str, shd);
    draw_text(x, y, str, col);
    draw_text(x + 1, y, str, col);
}

/* ====== CURSOR (XP style - white with black outline) ====== */
static const uint8_t cursor_bmp[16][12] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,1,1,1,1,1,0},
    {1,2,2,1,2,2,1,0,0,0,0,0},
    {1,2,1,0,1,2,2,1,0,0,0,0},
    {1,1,0,0,1,2,2,1,0,0,0,0},
    {1,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,1,1,1,0,0,0,0},
};
static void draw_cursor(int mx, int my) {
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 12; x++) {
            uint8_t p = cursor_bmp[y][x];
            if (p == 1)      putpixel(mx+x, my+y, COL_BLACK);
            else if (p == 2) putpixel(mx+x, my+y, COL_WHITE);
        }
}

/* ====== WINDOW MANAGER ====== */
typedef enum { APP_NONE=0, APP_ABOUT, APP_CALC, APP_FILES, APP_NOTEPAD, APP_IMGVIEW, APP_GL3D, APP_TERM, APP_ELF_GL } app_type_t;

typedef struct {
    bool active;
    char title[MAX_WIN_TITLE];
    int x, y, w, h;
    app_type_t app;
    bool focused;
    union {
        struct { int32_t accum, operand; char op; bool has_op, entered; char display[16]; } calc;
        struct { char text[1024]; int len, cursor, scroll; } note;
        struct { char cwd[64]; int scroll; } files;
        struct { image_t img; char path[64]; } imgview;
        struct { float ax, ay, dist; int scene; bool wire, autorot; } gl3d;
        struct { char input[256]; int ilen, icursor, scroll; char cwd[64]; int hist_idx; } term;
        struct { uint32_t owner_pid; } elf_gl;
    };
} window_t;

static window_t windows[MAX_WINDOWS];
static int focus_idx = -1;
static bool start_menu_open = false;
static bool gui_running = true;
static bool dragging = false;

/* ====== DIRTY TRACKING ======
 * Skip expensive full-screen redraws + 8MB blit when nothing changed.
 * Mark dirty on: mouse movement, keyboard input, window changes, ELF frame updates.
 */
static bool needs_redraw = true;
static int  prev_mouse_x = -1, prev_mouse_y = -1;

static void gui_mark_dirty(void) { needs_redraw = true; }

/* GL3D window rendering */
#define GL_WIN_W 384
#define GL_WIN_H 280
static uint32_t* gl_pixbuf = NULL;
static bool gl_inited = false;

/* ====== ELF GUI WINDOW SUPPORT ====== */
/* Shared framebuffer between kernel and ELF processes */
#define MAX_ELF_GUI      4
#define ELF_FB_PAGES     ((ELF_GUI_FB_W * ELF_GUI_FB_H * 4 + 4095) / 4096)
#define ELF_FB_KVBASE    0x22000000   /* Kernel VA base for ELF framebuffers */

typedef struct {
    bool     active;
    uint32_t pid;
    uint32_t phys_pages[ELF_FB_PAGES];  /* Physical addresses of FB pages */
    uint32_t* fb_kaddr;                  /* Kernel virtual address */
    int      win_idx;                    /* Window index (or -1) */
} elf_gui_slot_t;

static elf_gui_slot_t elf_gui_slots[MAX_ELF_GUI];

/* Desktop wallpaper */
static uint32_t* wallpaper = NULL;
static bool wallpaper_loaded = false;
static int drag_idx = -1, drag_ox, drag_oy;

/* Start button hover state */
static bool start_btn_hover = false;

static int find_free_window(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) if (!windows[i].active) return i;
    return -1;
}
static void bring_to_front(int idx) {
    if (idx < 0 || !windows[idx].active) return;
    for (int i = 0; i < MAX_WINDOWS; i++) windows[i].focused = false;
    window_t tmp = windows[idx];
    for (int i = idx; i < MAX_WINDOWS - 1; i++) windows[i] = windows[i+1];
    windows[MAX_WINDOWS - 1] = tmp;
    for (int i = MAX_WINDOWS - 1; i >= 0; i--)
        if (windows[i].active) { windows[i].focused = true; focus_idx = i; return; }
    focus_idx = -1;
}
static int open_window(const char* title, int x, int y, int w, int h, app_type_t app) {
    serial_printf("GUI: open_window(%s, %d,%d, %dx%d, app=%d)\n", 
                  title, x, y, w, h, (int)app);
    
    int idx = find_free_window();
    serial_printf("GUI: find_free_window returned %d\n", idx);
    
    if (idx < 0) return -1;
    
    window_t* win = &windows[idx];
    serial_printf("GUI: clearing window struct at %p (size=%u)\n", 
                  win, (unsigned)sizeof(*win));
    
    memset(win, 0, sizeof(*win));
    
    serial_printf("GUI: setting window fields\n");
    win->active = true;
    strncpy(win->title, title, MAX_WIN_TITLE - 1);
    win->x = x; win->y = y; win->w = w; win->h = h; win->app = app;
    
    serial_printf("GUI: calling bring_to_front(%d)\n", idx);
    bring_to_front(idx);
    
    serial_printf("GUI: open_window complete, returning %d\n", focus_idx);
    gui_mark_dirty();
    return focus_idx;
}
static void close_window(int idx) {
    if (idx < 0 || idx >= MAX_WINDOWS) return;
    windows[idx].active = false;
    focus_idx = -1;
    for (int i = MAX_WINDOWS - 1; i >= 0; i--)
        if (windows[i].active) { windows[i].focused = true; focus_idx = i; break; }
    gui_mark_dirty();
}

/* ====== HELPERS ====== */
static void int_to_str(int32_t val, char* buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[16]; int i = 0; bool neg = false;
    if (val < 0) { neg = true; val = -val; }
    while (val > 0 && i < 14) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int j = 0; if (neg) buf[j++] = '-';
    while (--i >= 0) buf[j++] = tmp[i];
    buf[j] = '\0';
}

/* ====== XP WINDOW CHROME ====== */

/* Draw XP-style caption button (close, minimize, etc.) */
static void draw_caption_button(int x, int y, int w, int h,
                                 uint32_t top_col, uint32_t bot_col,
                                 uint32_t shine_col) {
    /* Rounded button with gradient */
    fill_gradient_v(x+1, y, w-2, h, top_col, bot_col);
    /* Top corners: skip pixels for rounding */
    putpixel(x+1, y, rgb_lerp(top_col, COL_BLACK, 128));
    putpixel(x+w-2, y, rgb_lerp(top_col, COL_BLACK, 128));
    /* Sides */
    draw_vline(x, y+1, h-2, rgb_darken(bot_col, 180));
    draw_vline(x+w-1, y+1, h-2, rgb_darken(bot_col, 180));
    /* Bottom */
    draw_hline(x+1, y+h-1, w-2, rgb_darken(bot_col, 160));
    putpixel(x+1, y+h-1, rgb_lerp(bot_col, COL_BLACK, 128));
    putpixel(x+w-2, y+h-1, rgb_lerp(bot_col, COL_BLACK, 128));
    /* Shine line at top */
    draw_hline(x+2, y+1, w-4, shine_col);
    /* Inner left highlight */
    draw_vline(x+1, y+2, h-4, rgb_lighten(top_col, 30));
}

/* Draw the X glyph for close button */
static void draw_close_x(int cx, int cy, uint32_t col) {
    /* 7x7 X glyph */
    for (int i = 0; i < 7; i++) {
        putpixel(cx + i, cy + i, col);
        putpixel(cx + i + 1, cy + i, col);
        putpixel(cx + 6 - i, cy + i, col);
        putpixel(cx + 7 - i, cy + i, col);
    }
}

static void draw_window(window_t* win) {
    int x = win->x, y = win->y, w = win->w, h = win->h;

    /* ---- XP Drop shadow (soft, multi-layer) ---- */
    fill_rect_alpha(x + 5, y + 5, w, h, COL_SHADOW, 40);
    fill_rect_alpha(x + 4, y + 4, w, h, COL_SHADOW, 30);
    fill_rect_alpha(x + 3, y + 3, w + 1, h + 1, COL_SHADOW, 20);

    /* ---- Outer border (blue for active, gray-blue for inactive) ---- */
    uint32_t border_col = win->focused ? COL_WINBORDER_OUT : COL_IBORDER_OUT;
    /* Top border - rounded corners */
    draw_hline(x + 3, y, w - 6, border_col);
    draw_hline(x + 2, y + 1, w - 4, border_col);
    putpixel(x + 1, y + 2, border_col);
    putpixel(x + w - 2, y + 2, border_col);
    /* Left/Right borders */
    draw_vline(x, y + 3, h - 6, border_col);
    draw_vline(x + w - 1, y + 3, h - 6, border_col);
    /* Bottom border - rounded corners */
    draw_hline(x + 3, y + h - 1, w - 6, border_col);
    draw_hline(x + 2, y + h - 2, w - 4, border_col);
    putpixel(x + 1, y + h - 3, border_col);
    putpixel(x + w - 2, y + h - 3, border_col);

    /* Inner border line */
    uint32_t inner_brd = win->focused ? RGB(0, 80, 210) : RGB(140, 160, 190);
    draw_hline(x + 3, y + 1, w - 6, inner_brd);
    draw_vline(x + 1, y + 3, h - 6, inner_brd);
    draw_vline(x + w - 2, y + 3, h - 6, inner_brd);
    draw_hline(x + 3, y + h - 2, w - 6, inner_brd);

    /* Fill window body */
    fill_rect(x + 2, y + 3, w - 4, h - 6, COL_WINBODY);

    /* ---- Title bar: XP multi-stop blue gradient ---- */
    int tb_x = x + 2, tb_y = y + 3, tb_w = w - 4, tb_h = TITLEBAR_H;
    if (win->focused) {
        fill_gradient_v4(tb_x, tb_y, tb_w, tb_h,
                         COL_TITLE_T1, COL_TITLE_T2, COL_TITLE_T3, COL_TITLE_T4);
        /* Top shine highlight */
        draw_hline(tb_x + 1, tb_y, tb_w - 2, COL_TITLE_SHINE);
    } else {
        fill_gradient_v4(tb_x, tb_y, tb_w, tb_h,
                         COL_ITITLE_T1, COL_ITITLE_T2, COL_ITITLE_T3, COL_ITITLE_T4);
    }

    /* Title text - bold with shadow for XP look */
    draw_text_bold_shadow(x + 10, y + 9, win->title,
                          win->focused ? COL_TITLETXT : COL_ITITLETXT,
                          win->focused ? COL_TITLETXT_SHD : RGB(70, 80, 100));

    /* ---- Caption buttons (XP: Close is red, others are blue) ---- */
    int btn_w = 21, btn_h = 16;
    /* Close button [X] - red */
    int cbx = x + w - btn_w - 6, cby = y + 6;
    draw_caption_button(cbx, cby, btn_w, btn_h,
                        COL_CLOSE_TOP, COL_CLOSE_BOT, COL_CLOSE_SHINE);
    draw_close_x(cbx + 7, cby + 5, COL_WHITE);
    /* Shadow for X */
    draw_close_x(cbx + 8, cby + 6, RGB(120, 20, 10));

    /* ---- Client area border (sunken) ---- */
    int cl_x = x + 4, cl_y = y + TITLEBAR_H + 4;
    int cl_w = w - 8, cl_h = h - TITLEBAR_H - 8;
    draw_hline(cl_x, cl_y, cl_w, COL_BTNSHADOW);
    draw_vline(cl_x, cl_y, cl_h, COL_BTNSHADOW);
    draw_hline(cl_x, cl_y + cl_h - 1, cl_w, COL_BTNHI);
    draw_vline(cl_x + cl_w - 1, cl_y, cl_h, COL_BTNHI);
}

/* ====== APP: ABOUT ====== */
static void draw_app_about(window_t* win) {
    int x = win->x + 16, y = win->y + TITLEBAR_H + 16;

    /* XP-style about box with icon area */
    /* Blue gradient banner at top */
    fill_gradient_v(win->x + 5, win->y + TITLEBAR_H + 5,
                    win->w - 10, 40, RGB(30, 80, 180), RGB(60, 130, 220));
    draw_text_bold_shadow(x + 6, y + 6, "MicroKernel OS", COL_WHITE, RGB(0, 0, 80));
    draw_text_shadow(x + 6, y + 20, "Version 0.3", RGB(200, 220, 255), RGB(0, 0, 80));

    y += 52;
    draw_text(x, y,      "x86 Microkernel with", COL_TEXT);
    draw_text(x, y + 14, "Graphical Desktop Environment", COL_TEXT);

    char resbuf[40];
    ksnprintf(resbuf, sizeof(resbuf), "%ux%u 32-bit True Color", GFX_W, GFX_H);
    draw_text(x, y + 34, resbuf, COL_GRAY);
    draw_text(x, y + 48, "Bochs VBE, 16.7M colors", COL_GRAY);

    draw_text(x, y + 68, "Windows XP Luna Theme", RGB(0, 80, 180));

    /* OK button */
    int bx = x + 70, by = y + 92;
    draw_xp_button(bx, by, 75, 23, COL_BTNFACE, false);
    draw_text_centered(bx, by, 75, 23, "OK", COL_TEXT);
    /* XP focus rectangle around default button */
    draw_rect(bx - 1, by - 1, 77, 25, COL_BLACK);
}
static void click_about(window_t* win, int rx, int ry) {
    if (rx >= 86 && rx < 161 && ry >= TITLEBAR_H + 160 && ry < TITLEBAR_H + 183)
        close_window((int)(win - windows));
}

/* ====== APP: CALCULATOR ====== */
static const char calc_btns[4][4] = {
    {'7','8','9','/'}, {'4','5','6','*'},
    {'1','2','3','-'}, {'C','0','=','+'},
};
static void calc_update_display(window_t* win) {
    int_to_str(win->calc.entered ? win->calc.operand : win->calc.accum, win->calc.display);
}
static void do_calc_op(window_t* win) {
    switch (win->calc.op) {
        case '+': win->calc.accum += win->calc.operand; break;
        case '-': win->calc.accum -= win->calc.operand; break;
        case '*': win->calc.accum *= win->calc.operand; break;
        case '/': if (win->calc.operand) win->calc.accum /= win->calc.operand; break;
    }
}
static void draw_app_calc(window_t* win) {
    int bx = win->x + 12, by = win->y + TITLEBAR_H + 10;

    /* XP calculator display */
    draw_xp_editbox(bx, by, win->w - 24, 22);
    int tw = text_width(win->calc.display);
    draw_text(bx + win->w - 30 - tw, by + 8, win->calc.display, COL_TEXT);
    by += 30;

    int btnw = 38, btnh = 26, gap = 3;
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++) {
            int xx = bx + col * (btnw + gap);
            int yy = by + row * (btnh + gap);
            /* Operator buttons get a slightly blue tint in XP calc */
            uint32_t face = COL_BTNFACE;
            char key = calc_btns[row][col];
            if (key == '/' || key == '*' || key == '-' || key == '+')
                face = RGB(230, 230, 240);
            else if (key == '=' || key == 'C')
                face = RGB(230, 225, 215);
            draw_xp_button(xx, yy, btnw, btnh, face, false);
            char s[2] = { key, 0 };
            draw_text_centered(xx, yy, btnw, btnh, s, COL_TEXT);
        }
}
static void click_calc(window_t* win, int rx, int ry) {
    int bx = 12, by = TITLEBAR_H + 40;
    int btnw = 38, btnh = 26, gap = 3;
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++) {
            int xx = bx + col * (btnw + gap); int yy = by + row * (btnh + gap);
            if (rx >= xx && rx < xx + btnw && ry >= yy && ry < yy + btnh) {
                char key = calc_btns[row][col];
                if (key >= '0' && key <= '9') {
                    if (!win->calc.entered) { win->calc.operand = 0; win->calc.entered = true; }
                    win->calc.operand = win->calc.operand * 10 + (key - '0');
                } else if (key == 'C') {
                    win->calc.accum = 0; win->calc.operand = 0;
                    win->calc.op = 0; win->calc.has_op = false; win->calc.entered = false;
                } else if (key == '=') {
                    if (win->calc.has_op) { do_calc_op(win); win->calc.has_op = false; win->calc.entered = false; }
                } else {
                    if (win->calc.has_op && win->calc.entered) do_calc_op(win);
                    else if (!win->calc.has_op)
                        win->calc.accum = win->calc.entered ? win->calc.operand : win->calc.accum;
                    win->calc.op = key; win->calc.has_op = true; win->calc.entered = false;
                }
                calc_update_display(win); return;
            }
        }
}

/* ====== APP: FILES ====== */

/* Returns the relative path within the disk mount, or NULL if not a disk path.
 * Sets *drive to 0 for /disk, 1 for /disk2 */
static const char* gui_disk_path(const char* path, int* drive) {
    /* Must check /disk2 before /disk since /disk is a prefix of /disk2 */
    if (strncmp(path, "/disk2", 6) == 0) {
        if (drive) *drive = 1;
        if (path[6] == '\0') return "/";
        if (path[6] == '/') return path + 6;
    }
    if (strncmp(path, "/disk", 5) == 0) {
        if (drive) *drive = 0;
        if (path[5] == '\0') return "/";
        if (path[5] == '/') return path + 5;
    }
    if (drive) *drive = -1;
    return NULL;
}

typedef struct { char name[48]; bool is_dir; bool is_image; } gui_file_entry_t;
#define GUI_MAX_ENTRIES 64

static int gui_get_entries(const char* cwd, gui_file_entry_t* entries) {
    int drive = -1;
    const char* dp = gui_disk_path(cwd, &drive);
    int count = 0;

    if (dp && drive >= 0) {
        /* Check if FAT16 is on this drive */
        if (fat16_is_mounted() && fat16_get_drive_idx() == drive) {
            fat16_dirent_t fents[GUI_MAX_ENTRIES];
            int n = fat16_list_dir(dp, fents, GUI_MAX_ENTRIES);
            for (int i = 0; i < n && count < GUI_MAX_ENTRIES; i++) {
                strncpy(entries[count].name, fents[i].name, 47);
                entries[count].name[47] = '\0';
                entries[count].is_dir = fents[i].is_dir;
                entries[count].is_image = !fents[i].is_dir && image_is_image(fents[i].name);
                count++;
            }
        }
        /* Check if NTFS is on this drive */
        else if (ntfs_is_mounted() && ntfs_get_drive_idx() == drive) {
            ntfs_dirent_t nents[GUI_MAX_ENTRIES];
            int n = ntfs_list_dir(dp, nents, GUI_MAX_ENTRIES);
            for (int i = 0; i < n && count < GUI_MAX_ENTRIES; i++) {
                strncpy(entries[count].name, nents[i].name, 47);
                entries[count].name[47] = '\0';
                entries[count].is_dir = nents[i].is_dir;
                entries[count].is_image = !nents[i].is_dir && image_is_image(nents[i].name);
                count++;
            }
        }
    } else {
        int32_t dir = ramfs_find(cwd);
        if (dir < 0) return 0;
        for (int i = 0; i < RAMFS_MAX_FILES && count < GUI_MAX_ENTRIES; i++) {
            ramfs_node_t* node = ramfs_get_node(i);
            if (!node || node->parent != dir) continue;
            strncpy(entries[count].name, node->name, 47);
            entries[count].name[47] = '\0';
            entries[count].is_dir = (node->type == RAMFS_DIR);
            entries[count].is_image = !entries[count].is_dir && image_is_image(node->name);
            count++;
        }
    }
    return count;
}

static void draw_app_files(window_t* win) {
    int x = win->x + 8, y = win->y + TITLEBAR_H + 8;
    int cw = win->w - 16, ch = win->h - TITLEBAR_H - 14;

    /* XP-style address bar */
    draw_text(x + 2, y + 4, "Address:", COL_TEXT);
    draw_xp_editbox(x + 44, y, cw - 44, 16);
    draw_text_clipped(x + 48, y + 5, win->files.cwd, COL_TEXT, cw - 52);
    y += 22;

    /* File list area */
    draw_xp_editbox(x, y, cw, ch - 26);

    gui_file_entry_t entries[GUI_MAX_ENTRIES];
    int entry_count = gui_get_entries(win->files.cwd, entries);

    int line = 0, max_lines = (ch - 32) / 12;
    /* XP uses alternating row colors */
    if (strcmp(win->files.cwd, "/") != 0) {
        if (line >= win->files.scroll && line < win->files.scroll + max_lines) {
            int ly = y + 4 + (line - win->files.scroll) * 12;
            /* Alternating background */
            if ((line - win->files.scroll) % 2 == 1)
                fill_rect(x + 2, ly - 1, cw - 4, 12, RGB(245, 245, 250));
            draw_text(x + 22, ly, "..", COL_DIRBLUE);
            /* Folder icon (tiny) */
            fill_rect(x + 6, ly + 1, 8, 6, RGB(255, 220, 80));
            fill_rect(x + 6, ly, 5, 2, RGB(255, 200, 50));
            draw_rect(x + 6, ly, 10, 8, RGB(180, 140, 30));
        }
        line++;
    }
    for (int i = 0; i < entry_count; i++) {
        if (line >= win->files.scroll && line < win->files.scroll + max_lines) {
            int ly = y + 4 + (line - win->files.scroll) * 12;
            /* Alternating row background */
            if ((line - win->files.scroll) % 2 == 1)
                fill_rect(x + 2, ly - 1, cw - 4, 12, RGB(245, 245, 250));
            if (entries[i].is_dir) {
                /* Folder icon */
                fill_rect(x + 6, ly + 1, 8, 6, RGB(255, 220, 80));
                fill_rect(x + 6, ly, 5, 2, RGB(255, 200, 50));
                draw_rect(x + 6, ly, 10, 8, RGB(180, 140, 30));
                draw_text_clipped(x + 22, ly, entries[i].name, COL_DIRBLUE, cw - 28);
            } else if (entries[i].is_image) {
                /* Image icon (tiny) */
                fill_rect(x + 6, ly, 10, 8, RGB(200, 230, 200));
                draw_rect(x + 6, ly, 10, 8, RGB(100, 160, 100));
                draw_text_clipped(x + 22, ly, entries[i].name, RGB(0, 128, 0), cw - 28);
            } else {
                /* File icon */
                fill_rect(x + 7, ly, 8, 8, RGB(255, 255, 255));
                draw_rect(x + 7, ly, 8, 8, RGB(150, 150, 150));
                draw_hline(x + 9, ly + 2, 4, RGB(180, 180, 180));
                draw_hline(x + 9, ly + 4, 4, RGB(180, 180, 180));
                draw_text_clipped(x + 22, ly, entries[i].name, COL_TEXT, cw - 28);
            }
        }
        line++;
    }
}
static void open_image_viewer(const char* filepath);

static void click_files(window_t* win, int rx, int ry) {
    (void)rx;
    int by = TITLEBAR_H + 30;
    int ch = win->h - TITLEBAR_H - 14;
    int max_lines = (ch - 32) / 12;
    if (ry < by || ry >= by + max_lines * 12) return;
    int clicked = (ry - by) / 12 + win->files.scroll;

    int line = 0;
    if (strcmp(win->files.cwd, "/") != 0) {
        if (clicked == 0) {
            char* last = win->files.cwd;
            for (char* p = win->files.cwd; *p; p++) if (*p == '/') last = p;
            if (last == win->files.cwd) strcpy(win->files.cwd, "/"); else *last = '\0';
            win->files.scroll = 0; return;
        }
        line++;
    }

    gui_file_entry_t entries[GUI_MAX_ENTRIES];
    int entry_count = gui_get_entries(win->files.cwd, entries);

    for (int i = 0; i < entry_count; i++) {
        if (line == clicked) {
            if (entries[i].is_dir) {
                if (strcmp(win->files.cwd, "/") == 0)
                    strcat(win->files.cwd, entries[i].name);
                else {
                    strcat(win->files.cwd, "/");
                    strcat(win->files.cwd, entries[i].name);
                }
                win->files.scroll = 0;
                return;
            }
            if (entries[i].is_image) {
                char fpath[128];
                strcpy(fpath, win->files.cwd);
                if (strcmp(fpath, "/") != 0) strcat(fpath, "/");
                strcat(fpath, entries[i].name);
                serial_printf("GUI: click_files opening image '%s'\n", fpath);
                open_image_viewer(fpath);
                return;
            }
        }
        line++;
    }
}

/* ====== APP: NOTEPAD ====== */
static void draw_app_notepad(window_t* win) {
    int x = win->x + 6, y = win->y + TITLEBAR_H + 6;
    int cw = win->w - 12, ch = win->h - TITLEBAR_H - 12;

    /* XP Notepad: white edit area with proper inset border */
    draw_xp_editbox(x, y, cw, ch);

    int max_cols = (cw - 8) / FONT_W;
    int max_rows = (ch - 6) / FONT_H;
    int col = 0, row = -win->note.scroll;
    for (int i = 0; i < win->note.len && row < max_rows; i++) {
        char c = win->note.text[i];
        if (c == '\n') { row++; col = 0; continue; }
        if (col >= max_cols) { row++; col = 0; }
        if (row >= 0 && row < max_rows)
            draw_char(x + 4 + col * FONT_W, y + 3 + row * FONT_H, c, COL_TEXT);
        col++;
    }
    /* Blinking cursor */
    if (win->focused && (timer_get_ticks() / 30) % 2 == 0) {
        col = 0; row = -win->note.scroll;
        for (int i = 0; i < win->note.cursor; i++) {
            if (win->note.text[i] == '\n') { row++; col = 0; }
            else { col++; if (col >= max_cols) { row++; col = 0; } }
        }
        if (row >= 0 && row < max_rows)
            fill_rect(x + 4 + col * FONT_W, y + 3 + row * FONT_H, 1, FONT_H, COL_TEXT);
    }
}
static void key_notepad(window_t* win, char key) {
    if (key == '\b') {
        if (win->note.cursor > 0) {
            for (int i = win->note.cursor - 1; i < win->note.len - 1; i++)
                win->note.text[i] = win->note.text[i+1];
            win->note.cursor--; win->note.len--;
        }
    } else if ((uint8_t)key == KEY_LEFT) {
        if (win->note.cursor > 0) win->note.cursor--;
    } else if ((uint8_t)key == KEY_RIGHT) {
        if (win->note.cursor < win->note.len) win->note.cursor++;
    } else if ((key >= 32 && key != 127) || key == '\n') {
        if (win->note.len < 1022) {
            for (int i = win->note.len; i > win->note.cursor; i--)
                win->note.text[i] = win->note.text[i-1];
            win->note.text[win->note.cursor] = key;
            win->note.cursor++; win->note.len++;
            win->note.text[win->note.len] = '\0';
        }
    }
}

/* ====== APP: IMAGE VIEWER ====== */
static void draw_app_imgview(window_t* win) {
    int x = win->x + 5, y = win->y + TITLEBAR_H + 5;
    int cw = win->w - 10, ch = win->h - TITLEBAR_H - 10;

    if (!win->imgview.img.valid) {
        draw_sunken(x, y, cw, ch, RGB(32, 32, 32));
        draw_text(x + 10, y + 15, "Failed to load image", RGB(255, 80, 80));
        draw_text(x + 10, y + 30, win->imgview.path, RGB(200, 200, 200));
        if (win->imgview.img.width > 0) {
            char info[64];
            strcpy(info, "Source: ");
            char num[12];
            int n = win->imgview.img.width;
            int i = 0;
            if (n == 0) num[i++] = '0';
            else { char tmp[12]; int j = 0; while (n > 0) { tmp[j++] = '0' + n%10; n /= 10; } while (j > 0) num[i++] = tmp[--j]; }
            num[i] = '\0';
            strcat(info, num);
            strcat(info, "x");
            n = win->imgview.img.height; i = 0;
            if (n == 0) num[i++] = '0';
            else { char tmp[12]; int j = 0; while (n > 0) { tmp[j++] = '0' + n%10; n /= 10; } while (j > 0) num[i++] = tmp[--j]; }
            num[i] = '\0';
            strcat(info, num);
            strcat(info, " pixels");
            draw_text(x + 10, y + 45, info, RGB(180, 180, 180));
        }
        draw_text(x + 10, y + 65, "Try: convert img.png -resize", RGB(128, 128, 128));
        draw_text(x + 10, y + 77, "  small.png", RGB(128, 128, 128));
        return;
    }

    image_t* im = &win->imgview.img;
    int sw = cw - 4, sh = ch - 4;
    int iw = im->width, ih = im->height;
    int scale_x = (sw * 256) / iw;
    int scale_y = (sh * 256) / ih;
    int scale = (scale_x < scale_y) ? scale_x : scale_y;
    if (scale > 256) scale = 256;
    int dw = (iw * scale) / 256;
    int dh = (ih * scale) / 256;
    int ox = x + 2 + (sw - dw) / 2;
    int oy = y + 2 + (sh - dh) / 2;
    draw_sunken(x, y, cw, ch, RGB(32, 32, 32));
    for (int dy = 0; dy < dh; dy++) {
        int sy = (dy * ih) / dh;
        if (sy >= ih) sy = ih - 1;
        int py = oy + dy;
        if ((unsigned)py >= GFX_H) continue;
        for (int dx = 0; dx < dw; dx++) {
            int sx = (dx * iw) / dw;
            if (sx >= iw) sx = iw - 1;
            int px = ox + dx;
            if ((unsigned)px >= GFX_W) continue;
            backbuf[py * GFX_W + px] = im->pixels[sy * iw + sx];
        }
    }
    draw_text_clipped(x + 4, y + ch - 10, win->imgview.path, COL_WHITE, cw - 8);
}

/* ====== APP: TERMINAL ====== */

/* Global terminal output buffer */
#define TERM_OUT_SIZE  16384
#define TERM_HIST_SIZE 8
#define TERM_HIST_LEN  256
static char term_out[TERM_OUT_SIZE];
static int  term_out_len = 0;
static char term_history[TERM_HIST_SIZE][TERM_HIST_LEN];
static int  term_hist_count = 0;

/* Append text to terminal output buffer */
static void term_print(const char* s) {
    while (*s) {
        if (term_out_len >= TERM_OUT_SIZE - 1) {
            /* Shift buffer: discard first half */
            int half = TERM_OUT_SIZE / 2;
            /* Find start of a line near the midpoint */
            int cut = half;
            while (cut < term_out_len && term_out[cut] != '\n') cut++;
            if (cut < term_out_len) cut++;
            int remain = term_out_len - cut;
            memmove(term_out, term_out + cut, remain);
            term_out_len = remain;
        }
        term_out[term_out_len++] = *s++;
    }
    term_out[term_out_len] = '\0';
}

/* Printf-style into terminal - uses same va_list builtins as procfs */
#ifndef __TERM_VA_LIST
#define __TERM_VA_LIST
typedef __builtin_va_list term_va_list;
#define term_va_start(ap, last) __builtin_va_start(ap, last)
#define term_va_end(ap)         __builtin_va_end(ap)
#define term_va_arg(ap, type)   __builtin_va_arg(ap, type)
#endif

static void term_vprintf(const char* fmt, term_va_list ap) {
    char buf[512];
    int pos = 0, max = sizeof(buf);
    while (*fmt && pos < max - 1) {
        if (*fmt != '%') { buf[pos++] = *fmt++; continue; }
        fmt++;
        /* Parse flags and width */
        bool left_align = false;
        char pad_char = ' ';
        int width = 0;
        if (*fmt == '-') { left_align = true; fmt++; }
        if (*fmt == '0') { pad_char = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }

        /* Format the value into a temp buffer, then apply padding */
        char tmp[64];
        int tlen = 0;

        switch (*fmt) {
            case 's': {
                const char* s = term_va_arg(ap, const char*);
                if (!s) s = "(null)";
                tlen = strlen(s);
                /* Apply padding directly to output */
                if (!left_align && width > tlen) for (int i = 0; i < width - tlen && pos < max - 1; i++) buf[pos++] = ' ';
                for (int i = 0; i < tlen && pos < max - 1; i++) buf[pos++] = s[i];
                if (left_align && width > tlen) for (int i = 0; i < width - tlen && pos < max - 1; i++) buf[pos++] = ' ';
                fmt++;
                continue;
            }
            case 'd': {
                int32_t v = term_va_arg(ap, int32_t);
                bool neg = false;
                if (v < 0) { neg = true; v = -v; }
                if (v == 0) tmp[tlen++] = '0';
                else { char r[12]; int ri = 0; while (v > 0) { r[ri++] = '0' + (v % 10); v /= 10; } while (ri > 0) tmp[tlen++] = r[--ri]; }
                if (neg) { memmove(tmp + 1, tmp, tlen); tmp[0] = '-'; tlen++; }
                break;
            }
            case 'u': {
                uint32_t v = term_va_arg(ap, uint32_t);
                if (v == 0) tmp[tlen++] = '0';
                else { char r[12]; int ri = 0; while (v > 0) { r[ri++] = '0' + (v % 10); v /= 10; } while (ri > 0) tmp[tlen++] = r[--ri]; }
                break;
            }
            case 'x': {
                uint32_t v = term_va_arg(ap, uint32_t);
                const char* hex = "0123456789abcdef";
                if (v == 0) tmp[tlen++] = '0';
                else { char r[9]; int ri = 0; while (v > 0) { r[ri++] = hex[v & 0xF]; v >>= 4; } while (ri > 0) tmp[tlen++] = r[--ri]; }
                break;
            }
            case 'c':
                tmp[tlen++] = (char)term_va_arg(ap, int);
                break;
            case '%':
                tmp[tlen++] = '%';
                break;
            case '\0':
                goto done;
            default:
                tmp[tlen++] = *fmt;
                break;
        }
        tmp[tlen] = '\0';

        /* Apply width padding */
        if (!left_align && width > tlen) for (int i = 0; i < width - tlen && pos < max - 1; i++) buf[pos++] = pad_char;
        for (int i = 0; i < tlen && pos < max - 1; i++) buf[pos++] = tmp[i];
        if (left_align && width > tlen) for (int i = 0; i < width - tlen && pos < max - 1; i++) buf[pos++] = ' ';
        fmt++;
    }
done:
    buf[pos] = '\0';
    term_print(buf);
}

static void term_printf(const char* fmt, ...) {
    term_va_list ap;
    term_va_start(ap, fmt);
    term_vprintf(fmt, ap);
    term_va_end(ap);
}

/* Count lines in terminal output */
static int term_count_lines(int char_cols) {
    int lines = 0, col = 0;
    for (int i = 0; i < term_out_len; i++) {
        if (term_out[i] == '\n') { lines++; col = 0; }
        else { col++; if (col >= char_cols) { lines++; col = 0; } }
    }
    if (col > 0) lines++;
    return lines;
}

/* Terminal command processor */
static void term_resolve(const char* cwd, const char* path, char* out) {
    if (path[0] == '/') { strncpy(out, path, 127); out[127] = '\0'; return; }
    strncpy(out, cwd, 120); out[120] = '\0';
    if (strcmp(out, "/") != 0) strcat(out, "/");
    strcat(out, path);
    out[127] = '\0';
}

static void term_exec_cmd(window_t* win) {
    char* cmd = win->term.input;
    int len = win->term.ilen;
    if (len == 0) { term_print("\n"); return; }

    /* Echo command */
    term_printf("%s\n", cmd);

    /* Save to history */
    if (term_hist_count < TERM_HIST_SIZE) {
        strncpy(term_history[term_hist_count], cmd, TERM_HIST_LEN - 1);
        term_history[term_hist_count][TERM_HIST_LEN - 1] = '\0';
        term_hist_count++;
    } else {
        for (int i = 0; i < TERM_HIST_SIZE - 1; i++)
            strcpy(term_history[i], term_history[i + 1]);
        strncpy(term_history[TERM_HIST_SIZE - 1], cmd, TERM_HIST_LEN - 1);
    }
    win->term.hist_idx = term_hist_count;

    /* Parse into argc/argv */
    static char cmdbuf[256];
    strncpy(cmdbuf, cmd, 255); cmdbuf[255] = '\0';
    char* argv[16];
    int argc = 0;
    char* p = cmdbuf;
    while (*p && argc < 15) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }
    argv[argc] = NULL;
    if (argc == 0) return;

    /* ---- help ---- */
    if (strcmp(argv[0], "help") == 0) {
        term_print("Commands:\n");
        term_print("  ls [path]       - List files\n");
        term_print("  cd <path>       - Change directory\n");
        term_print("  pwd             - Print working directory\n");
        term_print("  cat <file>      - Display file contents\n");
        term_print("  exec <elf>      - Run ELF binary\n");
        term_print("    -io           - Grant I/O port access\n");
        term_print("    -vga          - Map VGA framebuffer\n");
        term_print("    -n <name>     - Set process name\n");
        term_print("    -p <0-3>      - Set priority\n");
        term_print("  ps              - List processes\n");
        term_print("  kill <pid>      - Kill a process\n");
        term_print("  free            - Show memory info\n");
        term_print("  echo <text>     - Print text\n");
        term_print("  clear           - Clear screen\n");
        term_print("  mkdir <name>    - Create directory\n");
        term_print("  rm <file>       - Delete file\n");
        term_print("  uname           - System info\n");
        term_print("  date            - Show date/time\n");
    }
    /* ---- clear ---- */
    else if (strcmp(argv[0], "clear") == 0) {
        term_out_len = 0; term_out[0] = '\0';
        win->term.scroll = 0;
    }
    /* ---- pwd ---- */
    else if (strcmp(argv[0], "pwd") == 0) {
        term_printf("%s\n", win->term.cwd);
    }
    /* ---- echo ---- */
    else if (strcmp(argv[0], "echo") == 0) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) term_print(" ");
            term_print(argv[i]);
        }
        term_print("\n");
    }
    /* ---- cd ---- */
    else if (strcmp(argv[0], "cd") == 0) {
        if (argc < 2) { strcpy(win->term.cwd, "/"); return; }
        char target[128];
        if (strcmp(argv[1], "..") == 0) {
            strcpy(target, win->term.cwd);
            char* last = target;
            for (char* q = target; *q; q++) if (*q == '/') last = q;
            if (last == target) strcpy(target, "/");
            else *last = '\0';
        } else {
            term_resolve(win->term.cwd, argv[1], target);
        }
        /* Verify it exists */
        ramfs_type_t ftype; uint32_t fsize;
        if (ramfs_stat(target, &ftype, &fsize) >= 0 && ftype == RAMFS_DIR) {
            strncpy(win->term.cwd, target, 63);
            win->term.cwd[63] = '\0';
        } else if (strcmp(target, "/") == 0) {
            strcpy(win->term.cwd, "/");
        } else {
            term_printf("cd: %s: No such directory\n", argv[1]);
        }
    }
    /* ---- ls ---- */
    else if (strcmp(argv[0], "ls") == 0) {
        char target[128];
        if (argc >= 2) term_resolve(win->term.cwd, argv[1], target);
        else strcpy(target, win->term.cwd);

        gui_file_entry_t entries[GUI_MAX_ENTRIES];
        (void)entries; /* suppress unused warning */
        int drive = -1;
        const char* dp = gui_disk_path(target, &drive);

        if (dp && drive >= 0) {
            if (fat16_is_mounted() && fat16_get_drive_idx() == drive) {
                fat16_dirent_t fents[GUI_MAX_ENTRIES];
                int n = fat16_list_dir(dp, fents, GUI_MAX_ENTRIES);
                for (int i = 0; i < n; i++) {
                    if (fents[i].is_dir)
                        term_printf("  \033[1m%s/\033[0m\n", fents[i].name);
                    else
                        term_printf("  %-20s %u\n", fents[i].name, fents[i].size);
                }
                if (n == 0) term_print("  (empty)\n");
            } else if (ntfs_is_mounted() && ntfs_get_drive_idx() == drive) {
                ntfs_dirent_t nents[GUI_MAX_ENTRIES];
                int n = ntfs_list_dir(dp, nents, GUI_MAX_ENTRIES);
                for (int i = 0; i < n; i++) {
                    if (nents[i].is_dir)
                        term_printf("  %s/\n", nents[i].name);
                    else
                        term_printf("  %-20s %u\n", nents[i].name, (uint32_t)nents[i].size);
                }
                if (n == 0) term_print("  (empty)\n");
            } else {
                term_print("  (no filesystem mounted on this drive)\n");
            }
        } else {
            /* RAMFS listing */
            int32_t dir = ramfs_find(target);
            if (dir < 0) { term_printf("ls: %s: Not found\n", target); return; }
            int found = 0;
            for (int i = 0; i < RAMFS_MAX_FILES; i++) {
                ramfs_node_t* node = ramfs_get_node(i);
                if (!node || node->parent != dir) continue;
                if (node->type == RAMFS_DIR)
                    term_printf("  %s/\n", node->name);
                else
                    term_printf("  %-20s %u\n", node->name, node->size);
                found++;
            }
            if (!found) term_print("  (empty)\n");
        }
    }
    /* ---- cat ---- */
    else if (strcmp(argv[0], "cat") == 0) {
        if (argc < 2) { term_print("Usage: cat <file>\n"); return; }
        char target[128];
        term_resolve(win->term.cwd, argv[1], target);
        ramfs_type_t ftype; uint32_t fsize;
        if (ramfs_stat(target, &ftype, &fsize) < 0) {
            term_printf("cat: %s: Not found\n", argv[1]); return;
        }
        if (ftype != RAMFS_FILE) { term_printf("cat: %s: Is a directory\n", argv[1]); return; }
        if (fsize > 4096) { term_printf("cat: %s: File too large (%u bytes, max 4096)\n", argv[1], fsize); return; }
        static char fbuf[4097];
        int32_t rd = ramfs_read(target, fbuf, fsize);
        if (rd > 0) { fbuf[rd] = '\0'; term_print(fbuf); if (fbuf[rd-1] != '\n') term_print("\n"); }
    }
    /* ---- exec ---- */
    else if (strcmp(argv[0], "exec") == 0) {
        if (argc < 2) {
            term_print("Usage: exec <path.elf> [-io] [-vga] [-n name] [-p pri]\n");
            return;
        }
        char target[128];
        term_resolve(win->term.cwd, argv[1], target);

        /* Parse exec options */
        const char* ename = NULL;
        bool io_priv = false;
        uint32_t flags = 0, priority = 10;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-io") == 0) io_priv = true;
            else if (strcmp(argv[i], "-vga") == 0) flags |= ELF_MAP_VGA;
            else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) ename = argv[++i];
            else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) { priority = atoi(argv[++i]); if (priority < 3) priority = 3; }
        }

        /* Derive name from filename */
        char namebuf[32];
        if (!ename) {
            const char* q = target; const char* base = target;
            while (*q) { if (*q == '/') base = q + 1; q++; }
            strncpy(namebuf, base, 31); namebuf[31] = '\0';
            size_t nl = strlen(namebuf);
            if (nl > 4 && strcmp(namebuf + nl - 4, ".elf") == 0) namebuf[nl - 4] = '\0';
            ename = namebuf;
        }

        /* Stat + read */
        ramfs_type_t ftype; uint32_t fsize;
        if (ramfs_stat(target, &ftype, &fsize) < 0) {
            term_printf("exec: %s: Not found\n", argv[1]); return;
        }
        if (ftype != RAMFS_FILE) { term_printf("exec: %s: Not a file\n", argv[1]); return; }
        if (fsize < sizeof(elf32_ehdr_t)) { term_printf("exec: Too small for ELF (%u bytes)\n", fsize); return; }

        uint8_t* buf = (uint8_t*)kmalloc(fsize);
        if (!buf) { term_printf("exec: Out of memory (need %u bytes)\n", fsize); return; }
        int32_t rd = ramfs_read(target, buf, fsize);
        if (rd <= 0) { kfree(buf); term_print("exec: Read failed\n"); return; }

        if (elf_validate(buf, (uint32_t)rd) != 0) {
            kfree(buf);
            term_printf("exec: %s: Not a valid ELF binary\n", argv[1]);
            return;
        }

        term_printf("Loading '%s' (%u bytes)...\n", ename, rd);
        int32_t pid = elf_load(buf, (uint32_t)rd, ename, priority, io_priv, flags);
        kfree(buf);

        if (pid < 0) { term_print("exec: Failed to load ELF\n"); return; }
        term_printf("[OK] Process '%s' started (PID %u", ename, pid);
        if (io_priv) term_print(", I/O");
        if (flags & ELF_MAP_VGA) term_print(", VGA");
        term_print(")\n");
    }
    /* ---- ps ---- */
    else if (strcmp(argv[0], "ps") == 0) {
        term_print("  PID  STATE      NAME\n");
        term_print("  ---  ---------  --------------------\n");
        task_t* tasks = task_get_all();
        for (int i = 0; i < MAX_TASKS; i++) {
            if (!tasks[i].active) continue;
            const char* st;
            switch (tasks[i].state) {
                case TASK_READY:      st = "ready";   break;
                case TASK_RUNNING:    st = "running"; break;
                case TASK_SLEEPING:   st = "sleeping"; break;
                case TASK_BLOCKED:    st = "blocked"; break;
                case TASK_TERMINATED: st = "zombie";  break;
                default:              st = "???";     break;
            }
            term_printf("  %-4u %-10s %s%s\n", tasks[i].id, st, tasks[i].name,
                        tasks[i].is_user ? " [user]" : "");
        }
    }
    /* ---- kill ---- */
    else if (strcmp(argv[0], "kill") == 0) {
        if (argc < 2) { term_print("Usage: kill <pid>\n"); return; }
        uint32_t pid = atoi(argv[1]);
        if (pid <= 1) { term_print("kill: Cannot kill kernel tasks\n"); return; }
        task_t* t = task_get_by_pid(pid);
        if (!t || !t->active) { term_printf("kill: PID %u not found\n", pid); return; }
        term_printf("Killing '%s' (PID %u)\n", t->name, pid);
        task_kill(pid);
    }
    /* ---- free ---- */
    else if (strcmp(argv[0], "free") == 0) {
        uint32_t free_p = pmm_get_free_pages();
        uint32_t total_p = pmm_get_total_pages();
        uint32_t used_p = total_p - free_p;
        term_printf("  Physical: %uMB total, %uMB used, %uMB free\n",
                    total_p * 4 / 1024, used_p * 4 / 1024, free_p * 4 / 1024);
        term_printf("  Pages:    %u total, %u used, %u free\n", total_p, used_p, free_p);
        term_printf("  Tasks:    %u active\n", task_count());
    }
    /* ---- mkdir ---- */
    else if (strcmp(argv[0], "mkdir") == 0) {
        if (argc < 2) { term_print("Usage: mkdir <name>\n"); return; }
        char target[128]; term_resolve(win->term.cwd, argv[1], target);
        if (ramfs_create(target, RAMFS_DIR) >= 0) term_printf("Created directory: %s\n", target);
        else term_printf("mkdir: Failed to create %s\n", argv[1]);
    }
    /* ---- rm ---- */
    else if (strcmp(argv[0], "rm") == 0) {
        if (argc < 2) { term_print("Usage: rm <file>\n"); return; }
        char target[128]; term_resolve(win->term.cwd, argv[1], target);
        if (ramfs_delete(target) >= 0) term_printf("Deleted: %s\n", target);
        else term_printf("rm: Failed to delete %s\n", argv[1]);
    }
    /* ---- uname ---- */
    else if (strcmp(argv[0], "uname") == 0) {
        term_print("MicroKernel OS v0.10.0 (i686) - Preemptive Multitasking Microkernel\n");
    }
    /* ---- date ---- */
    else if (strcmp(argv[0], "date") == 0) {
        rtc_time_t t;
        rtc_read(&t);
        /* Manual zero-padded formatting */
        char dbuf[32];
        dbuf[0] = '2'; dbuf[1] = '0';
        dbuf[2] = '0' + t.year / 10; dbuf[3] = '0' + t.year % 10;
        dbuf[4] = '-';
        dbuf[5] = '0' + t.month / 10; dbuf[6] = '0' + t.month % 10;
        dbuf[7] = '-';
        dbuf[8] = '0' + t.day / 10; dbuf[9] = '0' + t.day % 10;
        dbuf[10] = ' ';
        dbuf[11] = '0' + t.hour / 10; dbuf[12] = '0' + t.hour % 10;
        dbuf[13] = ':';
        dbuf[14] = '0' + t.minute / 10; dbuf[15] = '0' + t.minute % 10;
        dbuf[16] = ':';
        dbuf[17] = '0' + t.second / 10; dbuf[18] = '0' + t.second % 10;
        dbuf[19] = '\n'; dbuf[20] = '\0';
        term_print(dbuf);
    }
    /* ---- unknown ---- */
    else {
        /* Check if it's an ELF file by name */
        char target[128]; term_resolve(win->term.cwd, argv[0], target);
        ramfs_type_t ftype; uint32_t fsize;
        bool found_elf = false;
        if (ramfs_stat(target, &ftype, &fsize) >= 0 && ftype == RAMFS_FILE) {
            /* Try as ELF automatically */
            if (fsize >= sizeof(elf32_ehdr_t)) {
                uint8_t* buf = (uint8_t*)kmalloc(fsize);
                if (buf) {
                    int32_t rd = ramfs_read(target, buf, fsize);
                    if (rd > 0 && elf_validate(buf, (uint32_t)rd) == 0) {
                        /* Auto-exec */
                        const char* bn = argv[0];
                        for (const char* q = argv[0]; *q; q++) if (*q == '/') bn = q + 1;
                        char nb[32]; strncpy(nb, bn, 31); nb[31] = '\0';
                        size_t nl = strlen(nb);
                        if (nl > 4 && strcmp(nb + nl - 4, ".elf") == 0) nb[nl - 4] = '\0';
                        term_printf("Loading '%s'...\n", nb);
                        int32_t pid = elf_load(buf, (uint32_t)rd, nb, 2, false, 0);
                        if (pid >= 0) term_printf("[OK] Process '%s' started (PID %u)\n", nb, pid);
                        else term_print("Failed to load\n");
                        found_elf = true;
                    }
                    kfree(buf);
                }
            }
        }
        if (!found_elf)
            term_printf("%s: command not found\n", argv[0]);
    }
}

/* Draw terminal window */
static void draw_app_term(window_t* win) {
    int x = win->x + 4, y = win->y + TITLEBAR_H + 2;
    int cw = win->w - 8, ch = win->h - TITLEBAR_H - 4;

    /* Black terminal background */
    fill_rect(x, y, cw, ch, RGB(12, 12, 12));
    draw_rect(x - 1, y - 1, cw + 2, ch + 2, RGB(60, 60, 60));

    int char_cols = (cw - 8) / FONT_W;
    int char_rows = (ch - 20) / FONT_H;  /* Reserve bottom row for input */
    if (char_cols < 10) char_cols = 10;
    if (char_rows < 3) char_rows = 3;

    /* Build prompt string */
    char prompt[80];
    const char* user = login_current_user();
    ksnprintf(prompt, sizeof(prompt), "%s@micro:%s$ ", user ? user : "root", win->term.cwd);
    int prompt_len = strlen(prompt);

    /* Count total lines in output */
    int total_lines = term_count_lines(char_cols);

    /* Auto-scroll to bottom */
    int max_scroll = total_lines - char_rows + 1;
    if (max_scroll < 0) max_scroll = 0;
    if (win->term.scroll > max_scroll) win->term.scroll = max_scroll;

    /* Draw output buffer */
    int tx = x + 4, ty = y + 2;
    int cur_line = 0, col = 0;
    uint32_t text_col = RGB(200, 255, 200);  /* Light green */
    uint32_t dir_col = RGB(100, 200, 255);   /* Cyan for directories */

    for (int i = 0; i < term_out_len; i++) {
        char c = term_out[i];
        if (c == '\n') {
            cur_line++; col = 0; continue;
        }
        if (col >= char_cols) { cur_line++; col = 0; }
        int vis_row = cur_line - win->term.scroll;
        if (vis_row >= 0 && vis_row < char_rows) {
            draw_char(tx + col * FONT_W, ty + vis_row * FONT_H, c, text_col);
        }
        col++;
    }

    /* Draw input line at bottom of terminal */
    int input_y = y + ch - FONT_H - 4;
    /* Separator line */
    draw_hline(x + 2, input_y - 2, cw - 4, RGB(40, 60, 40));

    /* Draw prompt */
    uint32_t prompt_col = RGB(100, 255, 100);  /* Bright green prompt */
    for (int i = 0; i < prompt_len && i < char_cols; i++)
        draw_char(tx + i * FONT_W, input_y, prompt[i], prompt_col);

    /* Draw input text */
    int input_start = prompt_len;
    for (int i = 0; i < win->term.ilen && input_start + i < char_cols; i++)
        draw_char(tx + (input_start + i) * FONT_W, input_y, win->term.input[i], COL_WHITE);

    /* Blinking cursor */
    if (win->focused && (timer_get_ticks() / 30) % 2 == 0) {
        int cx = tx + (input_start + win->term.icursor) * FONT_W;
        fill_rect(cx, input_y, FONT_W, FONT_H, RGB(200, 255, 200));
    }

    /* Scrollbar indicator */
    if (total_lines > char_rows) {
        int sb_x = x + cw - 6;
        int sb_h = ch - 22;
        fill_rect(sb_x, y + 2, 4, sb_h, RGB(30, 30, 30));
        int thumb_h = (char_rows * sb_h) / total_lines;
        if (thumb_h < 8) thumb_h = 8;
        int thumb_y = (max_scroll > 0) ? (win->term.scroll * (sb_h - thumb_h)) / max_scroll : 0;
        fill_rect(sb_x, y + 2 + thumb_y, 4, thumb_h, RGB(80, 120, 80));
    }
}

/* Handle terminal keypress */
static void key_terminal(window_t* win, char key) {
    if (key == '\n') {
        /* Execute command */
        win->term.input[win->term.ilen] = '\0';

        /* Print prompt + command to output */
        char prompt[80];
        ksnprintf(prompt, sizeof(prompt), "%s@micro:%s$ ",
                  login_current_user() ? login_current_user() : "root", win->term.cwd);
        term_print(prompt);

        term_exec_cmd(win);

        /* Reset input */
        win->term.ilen = 0;
        win->term.icursor = 0;
        win->term.input[0] = '\0';

        /* Auto-scroll to bottom */
        win->term.scroll = 99999;  /* Will be clamped in draw */
    }
    else if (key == '\b') {
        if (win->term.icursor > 0) {
            memmove(win->term.input + win->term.icursor - 1,
                    win->term.input + win->term.icursor,
                    win->term.ilen - win->term.icursor);
            win->term.icursor--;
            win->term.ilen--;
        }
    }
    else if ((uint8_t)key == KEY_LEFT) {
        if (win->term.icursor > 0) win->term.icursor--;
    }
    else if ((uint8_t)key == KEY_RIGHT) {
        if (win->term.icursor < win->term.ilen) win->term.icursor++;
    }
    else if ((uint8_t)key == KEY_UP) {
        /* Command history - up */
        if (win->term.hist_idx > 0) {
            win->term.hist_idx--;
            strcpy(win->term.input, term_history[win->term.hist_idx]);
            win->term.ilen = strlen(win->term.input);
            win->term.icursor = win->term.ilen;
        }
    }
    else if ((uint8_t)key == KEY_DOWN) {
        /* Command history - down */
        if (win->term.hist_idx < term_hist_count - 1) {
            win->term.hist_idx++;
            strcpy(win->term.input, term_history[win->term.hist_idx]);
            win->term.ilen = strlen(win->term.input);
            win->term.icursor = win->term.ilen;
        } else {
            win->term.hist_idx = term_hist_count;
            win->term.ilen = 0; win->term.icursor = 0;
            win->term.input[0] = '\0';
        }
    }
    else if (key == '\t') {
        /* Basic tab completion: try to complete filenames */
        /* For now, insert spaces */
        for (int i = 0; i < 4 && win->term.ilen < 254; i++) {
            memmove(win->term.input + win->term.icursor + 1,
                    win->term.input + win->term.icursor,
                    win->term.ilen - win->term.icursor);
            win->term.input[win->term.icursor] = ' ';
            win->term.icursor++;
            win->term.ilen++;
        }
    }
    else if ((key >= 32 && key < 127) && win->term.ilen < 254) {
        memmove(win->term.input + win->term.icursor + 1,
                win->term.input + win->term.icursor,
                win->term.ilen - win->term.icursor);
        win->term.input[win->term.icursor] = key;
        win->term.icursor++;
        win->term.ilen++;
    }
    win->term.input[win->term.ilen] = '\0';
}

/* Handle scroll click on terminal */
static void click_terminal(window_t* win, int rx, int ry) {
    int ch = win->h - TITLEBAR_H - 4;
    int char_cols = (win->w - 16) / FONT_W;
    int char_rows = (ch - 20) / FONT_H;
    int total_lines = term_count_lines(char_cols);
    int max_scroll = total_lines - char_rows + 1;
    if (max_scroll < 0) max_scroll = 0;

    /* Click in upper half = scroll up, lower half = scroll down */
    if (ry < TITLEBAR_H + ch / 2) {
        win->term.scroll -= 3;
        if (win->term.scroll < 0) win->term.scroll = 0;
    } else {
        win->term.scroll += 3;
        if (win->term.scroll > max_scroll) win->term.scroll = max_scroll;
    }
}

/* Initialize terminal window */
static void term_init_window(window_t* win) {
    memset(win->term.input, 0, sizeof(win->term.input));
    win->term.ilen = 0;
    win->term.icursor = 0;
    win->term.scroll = 99999;
    win->term.hist_idx = term_hist_count;

    /* Set initial cwd to user home */
    char hdir[64];
    ksnprintf(hdir, sizeof(hdir), "/home/%s", login_current_user() ? login_current_user() : "root");
    strncpy(win->term.cwd, hdir, 63);
    win->term.cwd[63] = '\0';

    /* Print welcome message if buffer is empty */
    if (term_out_len == 0) {
        term_print("MicroKernel Terminal v1.0\n");
        term_print("Type 'help' for available commands.\n\n");
    }
}

/* ====== APP: 3D VIEWER ====== */
static void gl3d_init_window(window_t* win) {
    win->gl3d.ax = 25; win->gl3d.ay = 0; win->gl3d.dist = 5;
    win->gl3d.scene = 0; win->gl3d.wire = false; win->gl3d.autorot = true;
    if (!gl_pixbuf) {
        gl_pixbuf = (uint32_t*)kmalloc(GL_WIN_W * GL_WIN_H * sizeof(uint32_t));
    }
    if (!gl_inited && gl_pixbuf) {
        glInit(gl_pixbuf, GL_WIN_W, GL_WIN_H);
        gl_inited = true;
    }
}

static void draw_app_gl3d(window_t* win) {
    int x = win->x + 6, y = win->y + TITLEBAR_H + 4;
    int cw = win->w - 12, ch = win->h - TITLEBAR_H - 8;

    if (!gl_pixbuf || !gl_inited) {
        draw_sunken(x, y, cw, ch, RGB(0,0,0));
        draw_text(x+10, y+20, "GL init failed", RGB(255,80,80));
        return;
    }

    if (win->gl3d.autorot) win->gl3d.ay += 1.2f;
    glSetTarget(gl_pixbuf, GL_WIN_W, GL_WIN_H);
    glViewport(0, 0, GL_WIN_W, GL_WIN_H);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);

    float lpos[] = {2,3,4,0}, lamb[] = {0.15f,0.15f,0.18f,1}, ldif[] = {0.9f,0.85f,0.8f,1};
    glLightfv(GL_LIGHT0, GL_POSITION, lpos);
    glLightfv(GL_LIGHT0, GL_AMBIENT,  lamb);
    glLightfv(GL_LIGHT0, GL_DIFFUSE,  ldif);

    glClearColor(0.06f, 0.06f, 0.14f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(60, (float)GL_WIN_W / GL_WIN_H, 0.1f, 100);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0, 0, -win->gl3d.dist);
    glRotatef(win->gl3d.ax, 1, 0, 0);
    glRotatef(win->gl3d.ay, 0, 1, 0);

    gl_draw_floor();
    gl_draw_axes();

    if (win->gl3d.wire) {
        glDisable(GL_LIGHTING); glPolygonMode(GL_LINE);
    } else {
        glEnable(GL_LIGHTING); glEnable(GL_LIGHT0); glPolygonMode(GL_FILL);
    }

    glBegin(GL_QUADS);
    switch (win->gl3d.scene) {
        case 0: gl_draw_cube(1.8f); break;
        case 1: gl_draw_torus(1.0f, 0.4f, 12, 8); break;
        case 2: gl_draw_pyramid(2.0f); break;
        case 3: gl_draw_sphere(1.2f, 12, 8); break;
    }
    glEnd();

    int bw = GL_WIN_W < cw ? GL_WIN_W : cw;
    int bh = GL_WIN_H < ch ? GL_WIN_H : ch;
    int ox = x + (cw - bw) / 2;
    int oy = y + (ch - bh) / 2;
    draw_sunken(x, y, cw, ch, RGB(0,0,0));
    for (int row = 0; row < bh; row++) {
        int dy = oy + row;
        if ((unsigned)dy >= GFX_H) continue;
        if ((unsigned)ox + bw > GFX_W) continue;
        memcpy(&backbuf[dy * GFX_W + ox],
               &gl_pixbuf[row * GL_WIN_W],
               bw * sizeof(uint32_t));
    }

    const char* names[] = {"Cube","Torus","Pyramid","Sphere"};
    draw_text_clipped(x+4, y+ch-10, names[win->gl3d.scene], RGB(0,200,80), cw-8);
    if (win->gl3d.wire) draw_text(x+cw-36, y+ch-10, "WIRE", RGB(200,200,0));
}

static void key_gl3d(window_t* win, char key) {
    switch ((unsigned char)key) {
        case ' ': win->gl3d.scene = (win->gl3d.scene + 1) % 4; break;
        case 'w': win->gl3d.wire = !win->gl3d.wire; break;
        case 'r': win->gl3d.autorot = !win->gl3d.autorot; break;
        case '+': case '=': win->gl3d.dist -= 0.5f; if (win->gl3d.dist < 2) win->gl3d.dist = 2; break;
        case '-': win->gl3d.dist += 0.5f; if (win->gl3d.dist > 15) win->gl3d.dist = 15; break;
        case 'h': case KEY_LEFT:  win->gl3d.ay -= 10; break;
        case 'l': case KEY_RIGHT: win->gl3d.ay += 10; break;
        case 'k': case KEY_UP:    win->gl3d.ax -= 8; break;
        case 'j': case KEY_DOWN:  win->gl3d.ax += 8; break;
        default: break;
    }
}



// src/gui.c

static void safe_copy_user_string(char* dest, const char* user_ptr, int max) {
    // We can't read user_ptr directly if it's in another PD.
    // For now, since the shell is launching it, we'll assume a default title
    // or handle it via kernel-side task names.
    strncpy(dest, "User App", max);
}

// src/gui.c

uint32_t gui_elf_win_open(uint32_t pid, uint32_t* page_dir, const char* title) {
    int si = -1;
    for (int i = 0; i < MAX_ELF_GUI; i++) {
        if (!elf_gui_slots[i].active) { si = i; break; }
    }
    if (si < 0) return 0;

    // 1. Open the window
    int idx = open_window("User App", 150, 100, ELF_GUI_FB_W + 14, ELF_GUI_FB_H + TITLEBAR_H + 10, APP_ELF_GL);
    if (idx < 0) return 0;

    // 2. CRITICAL: Use focus_idx. open_window calls bring_to_front, 
    // which moves the window to index focus_idx (usually 7).
    window_t* win = &windows[focus_idx];
    win->elf_gl.owner_pid = pid;
    
    task_t* t = task_get_by_pid(pid);
    if (t) strncpy(win->title, t->name, MAX_WIN_TITLE-1);

    elf_gui_slot_t* slot = &elf_gui_slots[si];
    slot->pid = pid;
    slot->win_idx = focus_idx;

    for (int i = 0; i < ELF_FB_PAGES; i++) {
        uint32_t phys = (uint32_t)pmm_alloc_page();
        slot->phys_pages[i] = phys;
        
        // 3. User mapping for the ELF process (writes pixels)
        uint32_t user_va = ELF_GUI_FB_UVADDR + (i * 4096);
        paging_map_user(page_dir, user_va, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
        
        // 4. Kernel mapping for the Compositor/Shell (reads pixels)
        // CRITICAL: Added PAGE_USER so the Shell (Ring 3) can read this!
        uint32_t kern_va = ELF_FB_KVBASE + (si * ELF_FB_PAGES * 4096) + (i * 4096);
        paging_map_page(kern_va, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
    }
    
    slot->fb_kaddr = (uint32_t*)(ELF_FB_KVBASE + si * (ELF_FB_PAGES * PAGE_SIZE));
    slot->active = true;
    
    return ELF_GUI_FB_UVADDR;
}

static void draw_app_elf_gl(window_t* win) {
    int x = win->x + 6, y = win->y + TITLEBAR_H + 4;
    int cw = win->w - 12, ch = win->h - TITLEBAR_H - 8;

    elf_gui_slot_t* slot = NULL;
    for (int i = 0; i < MAX_ELF_GUI; i++) {
        if (elf_gui_slots[i].active && elf_gui_slots[i].pid == win->elf_gl.owner_pid) {
            slot = &elf_gui_slots[i];
            break;
        }
    }

    if (!slot || !slot->fb_kaddr) {
        fill_rect(x, y, cw, ch, COL_BLACK);
        return;
    }

    uint32_t* src_fb = slot->fb_kaddr;
    int src_w = ELF_GUI_FB_W;
    int src_h = ELF_GUI_FB_H;

    /* Check if virgl has a display backing — always prefer it over CPU fb */
    uint32_t* virgl_backing = virgl_get_display_backing();
    if (virgl_backing) {
        src_fb = virgl_backing;
        virgl_ctx_t* vctx = virgl_get_ctx();
        if (vctx && vctx->fb_width > 0) {
            src_w = vctx->fb_width;
            src_h = vctx->fb_height;
        }
    }

    /*
     * Blit src_fb to window content area with nearest-neighbor scaling.
     * GPU framebuffer may be larger than window (e.g. 640x400 → 320x200 window).
     */
    for (int j = 0; j < ch; j++) {
        int dy = y + j;
        if ((unsigned)dy >= GFX_H) continue;

        /* Map destination row to source row */
        int src_y = (src_h > ch) ? (j * src_h / ch) : j;
        if (src_y >= src_h) continue;

        uint32_t* dst_line = &backbuf[dy * GFX_W + x];
        uint32_t* src_line = &src_fb[src_y * src_w];

        if (src_w <= cw) {
            /* Source fits in window — direct copy */
            for (int i = 0; i < src_w && i < cw; i++) {
                dst_line[i] = src_line[i];
            }
        } else {
            /* Source wider than window — scale down */
            for (int i = 0; i < cw; i++) {
                int src_x = i * src_w / cw;
                if (src_x < src_w)
                    dst_line[i] = src_line[src_x];
            }
        }
    }
}



void gui_elf_win_present(uint32_t pid) {
    /*
     * The ELF app has finished rendering a frame via GPU3D.
     * The virgl display backing now has fresh pixel data.
     * Yield to let the GUI compositor run its render loop
     * and pick up the new frame from the display backing.
     */
    (void)pid;
    gui_mark_dirty();
    task_yield();
}

void gui_elf_win_close(uint32_t pid) {
    for (int i = 0; i < MAX_ELF_GUI; i++) {
        if (!elf_gui_slots[i].active || elf_gui_slots[i].pid != pid) continue;
        elf_gui_slot_t* slot = &elf_gui_slots[i];

        /* Close the window */
        if (slot->win_idx >= 0 && slot->win_idx < MAX_WINDOWS) {
            /* Find the actual window (index may have shifted due to bring_to_front) */
            for (int w = 0; w < MAX_WINDOWS; w++) {
                if (windows[w].active && windows[w].app == APP_ELF_GL &&
                    windows[w].elf_gl.owner_pid == pid) {
                    close_window(w);
                    break;
                }
            }
        }

        /* Unmap from kernel space and free physical pages */
        uint32_t kva = ELF_FB_KVBASE + i * (ELF_FB_PAGES * PAGE_SIZE);
        for (int p = 0; p < ELF_FB_PAGES; p++) {
            paging_unmap_page(kva + p * PAGE_SIZE);
            pmm_free_page((void*)slot->phys_pages[p]);
        }

        slot->active = false;
        slot->fb_kaddr = NULL;
        serial_printf("GUI: closed ELF window for PID %u\n", pid);
        return;
    }
}

void gui_elf_process_exited(uint32_t pid) {
    gui_elf_win_close(pid);
}

/* Helper: close ELF GL window and kill the owning process */
static void close_elf_gl_window(window_t* win) {
    uint32_t pid = win->elf_gl.owner_pid;
    /* Clean up the slot */
    for (int i = 0; i < MAX_ELF_GUI; i++) {
        if (!elf_gui_slots[i].active || elf_gui_slots[i].pid != pid) continue;
        elf_gui_slot_t* slot = &elf_gui_slots[i];
        uint32_t kva = ELF_FB_KVBASE + i * (ELF_FB_PAGES * PAGE_SIZE);
        for (int p = 0; p < ELF_FB_PAGES; p++) {
            paging_unmap_page(kva + p * PAGE_SIZE);
            pmm_free_page((void*)slot->phys_pages[p]);
        }
        slot->active = false;
        slot->fb_kaddr = NULL;
        break;
    }
    /* Kill the owning process */
    task_kill(pid);
    serial_printf("GUI: killed ELF process PID %u (window closed)\n", pid);
}

static void open_image_viewer(const char* filepath) {
    ramfs_type_t ftype;
    uint32_t fsize;
    serial_printf("GUI: open_image_viewer '%s'\n", filepath);
    if (ramfs_stat(filepath, &ftype, &fsize) < 0) {
        serial_printf("GUI: stat failed for '%s'\n", filepath);
        return;
    }
    if (ftype != RAMFS_FILE || fsize == 0) {
        serial_printf("GUI: not a file or empty: type=%d size=%u\n", ftype, fsize);
        return;
    }

    serial_printf("GUI: reading image '%s' (%u bytes)\n", filepath, fsize);
    static uint8_t file_data[10 * 1024 * 1024];
    if (fsize > sizeof(file_data)) {
        serial_printf("GUI: file too large %u > %u, truncating\n", fsize, (uint32_t)sizeof(file_data));
        fsize = sizeof(file_data);
    }
    int32_t got = ramfs_read(filepath, (char*)file_data, fsize);
    if (got <= 0) {
        serial_printf("GUI: read failed, got=%d\n", got);
        return;
    }
    serial_printf("GUI: read %d bytes, first bytes: %02x %02x %02x %02x\n",
                  got, file_data[0], file_data[1], file_data[2], file_data[3]);

    const char* name = filepath;
    for (const char* p = filepath; *p; p++)
        if (*p == '/') name = p + 1;

    char title[MAX_WIN_TITLE];
    strncpy(title, name, MAX_WIN_TITLE - 1);
    title[MAX_WIN_TITLE - 1] = '\0';

    image_t img;
    bool loaded = image_load(&img, file_data, (uint32_t)got, filepath);
    serial_printf("GUI: image_load result=%s valid=%s %dx%d\n",
                  loaded ? "OK" : "FAIL", img.valid ? "yes" : "no",
                  img.width, img.height);

    if (loaded) {
        int ww = img.width + 14;
        int wh = img.height + TITLEBAR_H + 14;
        if (ww > (int)GFX_W - 20) ww = GFX_W - 20;
        if (wh > (int)GFX_H - 60) wh = GFX_H - 60;  /* Leave room for taskbar */
        if (ww < 160) ww = 160;
        if (wh < 100) wh = 100;
        int wx = (GFX_W - ww) / 2;
        int wy = (GFX_H - TASKBAR_H - wh) / 2;
        int idx = open_window(title, wx, wy, ww, wh, APP_IMGVIEW);
        if (idx >= 0) {
            windows[idx].imgview.img = img;
            strncpy(windows[idx].imgview.path, filepath, 63);
        }
    } else {
        int idx = open_window(title, 100, 80, 400, 200, APP_IMGVIEW);
        if (idx >= 0) {
            windows[idx].imgview.img.valid = false;
            windows[idx].imgview.img.width = 0;
            windows[idx].imgview.img.height = 0;
            strncpy(windows[idx].imgview.path, filepath, 63);
            if (got >= 29 && file_data[0] == 137 && file_data[1] == 'P') {
                uint32_t pw = (file_data[16]<<24)|(file_data[17]<<16)|(file_data[18]<<8)|file_data[19];
                uint32_t ph = (file_data[20]<<24)|(file_data[21]<<16)|(file_data[22]<<8)|file_data[23];
                uint8_t pdepth = file_data[24];
                uint8_t pctype = file_data[25];
                uint8_t pintrl = file_data[28];
                (void)pctype;
                char err[64];
                if (pintrl != 0) strcpy(err, "Interlaced PNG unsupported");
                else if (pdepth != 8) strcpy(err, "Only 8-bit depth supported");
                else if (pw > IMG_MAX_W || ph > IMG_MAX_H) strcpy(err, "Image too large (max 2048)");
                else strcpy(err, "Decompression failed");
                strncpy(windows[idx].imgview.path, err, 63);
                windows[idx].imgview.img.width = (int)pw;
                windows[idx].imgview.img.height = (int)ph;
            }
        }
    }
}

/* ================================================================ */
/*                    DESKTOP & TASKBAR (XP LUNA)                   */
/* ================================================================ */

static void load_wallpaper(void) {
    if (wallpaper_loaded) return;

    static const char* names[] = {
        "/disk/background.png", "/disk/background.bmp", "/disk/background.tga",
        "/disk/wallpaper.png",  "/disk/wallpaper.bmp",  "/disk/wallpaper.tga",
        "/disk/bg.png",         "/disk/bg.bmp",         "/disk/bg.tga",
        "/disk/backgrou.png",   "/disk/backgrou.bmp",
        "/disk2/background.png", "/disk2/background.bmp", "/disk2/background.tga",
        "/disk2/wallpaper.png",  "/disk2/wallpaper.bmp",  "/disk2/wallpaper.tga",
        "/disk2/bg.png",         "/disk2/bg.bmp",         "/disk2/bg.tga",
        "/disk2/backgrou.png",   "/disk2/backgrou.bmp",
        "/home/root/background.png", "/home/root/background.bmp",
        NULL
    };

    ramfs_type_t ftype;
    uint32_t fsize = 0;
    const char* found = NULL;

    for (int i = 0; names[i]; i++) {
        if (ramfs_stat(names[i], &ftype, &fsize) >= 0 && ftype == RAMFS_FILE && fsize > 0) {
            found = names[i];
            serial_printf("GUI: wallpaper found '%s' size=%u\n", found, fsize);
            break;
        }
    }
    if (!found) { serial_printf("GUI: no wallpaper found on any disk\n"); return; }

    uint8_t* fdata = (uint8_t*)kmalloc(fsize);
    if (!fdata) { serial_printf("GUI: wallpaper kmalloc(%u) failed\n", fsize); return; }
    int32_t got = ramfs_read(found, (char*)fdata, fsize);
    serial_printf("GUI: wallpaper read got=%d bytes\n", got);
    if (got <= 0) { kfree(fdata); return; }

    image_t img;
    bool ok = image_load(&img, fdata, (uint32_t)got, found);
    kfree(fdata);
    if (!ok || !img.valid || img.width <= 0 || img.height <= 0) {
        serial_printf("GUI: wallpaper decode failed ok=%d valid=%d %dx%d\n",
                      ok, img.valid, img.width, img.height);
        return;
    }

    serial_printf("GUI: wallpaper decoded %dx%d, scaling to %ux%u\n",
                  img.width, img.height, GFX_W, GFX_H);

    int desk_h = GFX_H - TASKBAR_H;
    uint32_t wp_size = (uint32_t)GFX_W * desk_h * sizeof(uint32_t);
    wallpaper = (uint32_t*)kmalloc(wp_size);
    if (!wallpaper) return;

    int iw = img.width, ih = img.height;
    for (int y = 0; y < desk_h; y++) {
        int sy = (y * ih) / desk_h;
        if (sy >= ih) sy = ih - 1;
        for (int x = 0; x < GFX_W; x++) {
            int sx = (x * iw) / GFX_W;
            if (sx >= iw) sx = iw - 1;
            wallpaper[y * GFX_W + x] = img.pixels[sy * iw + sx];
        }
    }
    wallpaper_loaded = true;
}

static void draw_desktop(void) {
    int desk_h = GFX_H - TASKBAR_H;

    if (wallpaper_loaded && wallpaper) {
        uint32_t n = (uint32_t)GFX_W * desk_h;
        memcpy(backbuf, wallpaper, n * sizeof(uint32_t));
        return;
    }

    /* XP "Bliss"-inspired gradient: blue sky to lighter blue/green at bottom */
    /* Top: deep blue, mid: bright blue, bottom: green-ish horizon */
    for (int y = 0; y < desk_h; y++) {
        int off = y * GFX_W;
        int t = (y * 255) / (desk_h - 1);
        uint32_t col;
        if (t < 128) {
            /* Top half: deep blue to bright blue */
            int t2 = t * 2;
            col = rgb_lerp(RGB(58, 110, 165), RGB(90, 160, 210), t2);
        } else {
            /* Bottom half: bright blue to green-tinged horizon */
            int t2 = (t - 128) * 2;
            col = rgb_lerp(RGB(90, 160, 210), RGB(110, 185, 160), t2);
        }
        for (int x = 0; x < GFX_W; x++)
            backbuf[off + x] = col;
    }

    /* Rolling hills effect at the bottom */
    for (int x = 0; x < GFX_W; x++) {
        /* Simple sine-like hill using integer approximation */
        int hill_base = desk_h - desk_h / 5;
        /* Two overlapping hills */
        int h1 = (desk_h / 8) * (128 + 127 * ((x * 3 + 50) % 256 - 128) / 128) / 256;
        int h2 = (desk_h / 10) * (128 + 127 * ((x * 2 + 180) % 256 - 128) / 128) / 256;
        int hill_top = hill_base - h1 / 2 - h2 / 3;
        if (hill_top < hill_base - desk_h/4) hill_top = hill_base - desk_h/4;

        for (int y = hill_top; y < desk_h; y++) {
            if ((unsigned)y >= GFX_H) continue;
            int t = ((y - hill_top) * 255) / (desk_h - hill_top);
            uint32_t green = rgb_lerp(RGB(80, 160, 50), RGB(60, 130, 40), t);
            backbuf[y * GFX_W + x] = green;
        }
    }
}

/* ---- XP-Style Start Button ---- */
#define START_BTN_W 90
#define START_BTN_H 24

static void draw_start_button(int ty, bool pressed, bool hover) {
    int x = 2, y = ty + 3;
    int w = START_BTN_W, h = START_BTN_H;

    /* Green pill-shaped button */
    uint32_t top, bot, shine;
    if (pressed) {
        top = rgb_darken(COL_START_MID, 200);
        bot = rgb_darken(COL_START_BOT, 200);
        shine = rgb_darken(COL_START_SHINE, 180);
    } else if (hover) {
        top = COL_START_HOV_TOP;
        bot = COL_START_HOV_BOT;
        shine = RGB(140, 230, 140);
    } else {
        top = COL_START_TOP;
        bot = COL_START_BOT;
        shine = COL_START_SHINE;
    }

    /* Main gradient body */
    fill_gradient_v(x + 3, y, w - 6, h, top, bot);
    /* Left rounded cap */
    fill_gradient_v(x + 1, y + 2, 2, h - 4, top, bot);
    fill_gradient_v(x + 2, y + 1, 1, h - 2, top, bot);
    putpixel(x + 2, y + 1, rgb_lerp(top, COL_BLACK, 80));
    putpixel(x + 2, y + h - 2, rgb_lerp(bot, COL_BLACK, 80));
    /* Right rounded cap */
    fill_gradient_v(x + w - 3, y + 2, 2, h - 4, top, bot);
    fill_gradient_v(x + w - 3, y + 1, 1, h - 2, top, bot);
    putpixel(x + w - 3, y + 1, rgb_lerp(top, COL_BLACK, 80));
    putpixel(x + w - 3, y + h - 2, rgb_lerp(bot, COL_BLACK, 80));

    /* Outline */
    draw_hline(x + 3, y, w - 6, rgb_darken(COL_START_BOT, 160));
    draw_hline(x + 3, y + h - 1, w - 6, rgb_darken(COL_START_BOT, 120));
    draw_vline(x, y + 3, h - 6, rgb_darken(COL_START_BOT, 160));
    draw_vline(x + w - 1, y + 3, h - 6, rgb_darken(COL_START_BOT, 160));
    /* Corner pixels */
    putpixel(x + 1, y + 2, rgb_darken(COL_START_BOT, 160));
    putpixel(x + 2, y + 1, rgb_darken(COL_START_BOT, 160));
    putpixel(x + w - 2, y + 2, rgb_darken(COL_START_BOT, 160));
    putpixel(x + w - 3, y + 1, rgb_darken(COL_START_BOT, 160));
    putpixel(x + 1, y + h - 3, rgb_darken(COL_START_BOT, 160));
    putpixel(x + 2, y + h - 2, rgb_darken(COL_START_BOT, 160));
    putpixel(x + w - 2, y + h - 3, rgb_darken(COL_START_BOT, 160));
    putpixel(x + w - 3, y + h - 2, rgb_darken(COL_START_BOT, 160));

    /* Top shine line */
    draw_hline(x + 4, y + 1, w - 8, shine);
    draw_hline(x + 5, y + 2, w - 10, rgb_lerp(shine, top, 128));

    /* Windows flag icon (tiny 4-color squares) */
    int ix = x + 8, iy = y + 6;
    /* Red quad */
    fill_rect(ix, iy, 4, 4, RGB(255, 0, 0));
    /* Green quad */
    fill_rect(ix + 5, iy, 4, 4, RGB(0, 180, 0));
    /* Blue quad */
    fill_rect(ix, iy + 5, 4, 4, RGB(0, 80, 255));
    /* Yellow quad */
    fill_rect(ix + 5, iy + 5, 4, 4, RGB(255, 210, 0));
    /* Slight wave effect - shift right quads up by 1 */
    putpixel(ix + 5, iy - 1, RGB(0, 180, 0));
    putpixel(ix + 6, iy - 1, RGB(0, 180, 0));

    /* "start" text - bold white with shadow */
    draw_text_bold_shadow(x + 24, y + 8, "start", COL_WHITE, RGB(0, 60, 0));
}

/* ---- XP Taskbar ---- */
static void draw_taskbar(void) {
    int ty = GFX_H - TASKBAR_H;

    /* Main taskbar gradient: dark blue at bottom, lighter blue at top */
    fill_gradient_v4(0, ty, GFX_W, TASKBAR_H,
                     COL_TASKBAR_SHINE, COL_TASKBAR_TOP,
                     COL_TASKBAR_MID, COL_TASKBAR_BOT);

    /* Top highlight line */
    draw_hline(0, ty, GFX_W, RGB(60, 140, 240));
    draw_hline(0, ty + 1, GFX_W, RGB(50, 120, 220));

    /* Start button */
    draw_start_button(ty, start_menu_open, start_btn_hover);

    /* Task buttons area - separator */
    draw_vline(START_BTN_W + 6, ty + 4, TASKBAR_H - 8, RGB(10, 50, 120));
    draw_vline(START_BTN_W + 7, ty + 4, TASKBAR_H - 8, RGB(60, 130, 230));

    /* Task buttons */
    int bx = START_BTN_W + 12;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].active) continue;
        if (bx + 140 > GFX_W - 90) break;
        int bw = 136, bh = 22;
        int by = ty + 4;
        if (windows[i].focused) {
            /* Pressed/active button - lighter, sunken look */
            fill_gradient_v(bx, by, bw, bh,
                            RGB(30, 82, 165), RGB(18, 60, 130));
            /* Sunken edges */
            draw_hline(bx, by, bw, RGB(10, 40, 100));
            draw_vline(bx, by, bh, RGB(10, 40, 100));
            draw_hline(bx, by + bh - 1, bw, RGB(50, 110, 200));
            draw_vline(bx + bw - 1, by, bh, RGB(50, 110, 200));
            /* Inner highlight */
            fill_rect_alpha(bx + 1, by + 1, bw - 2, bh / 2, COL_WHITE, 25);
        } else {
            /* Raised button */
            fill_gradient_v(bx, by, bw, bh,
                            RGB(55, 120, 210), RGB(30, 85, 175));
            /* Raised edges */
            draw_hline(bx, by, bw, RGB(80, 150, 240));
            draw_vline(bx, by, bh, RGB(70, 140, 230));
            draw_hline(bx, by + bh - 1, bw, RGB(20, 60, 140));
            draw_vline(bx + bw - 1, by, bh, RGB(20, 60, 140));
            /* Top shine */
            fill_rect_alpha(bx + 1, by + 1, bw - 2, bh / 3, COL_WHITE, 30);
        }
        /* Button text */
        draw_text_clipped(bx + 6, by + 7, windows[i].title, COL_WHITE, bw - 12);
        bx += bw + 3;
    }

    /* ---- System tray / notification area ---- */
    int tray_w = 80;
    int tray_x = GFX_W - tray_w - 4;
    /* Tray separator */
    draw_vline(tray_x - 3, ty + 4, TASKBAR_H - 8, RGB(10, 50, 120));
    draw_vline(tray_x - 2, ty + 4, TASKBAR_H - 8, RGB(60, 130, 230));

    /* Tray background (slightly recessed) */
    fill_gradient_v(tray_x, ty + 4, tray_w, TASKBAR_H - 8,
                    RGB(15, 60, 145), RGB(10, 45, 120));

    /* Clock */
    uint32_t hrs, mins, secs;
    timer_get_uptime(&hrs, &mins, &secs);
    (void)secs;
    char tbuf[6];
    tbuf[0] = '0' + (hrs / 10) % 10; tbuf[1] = '0' + hrs % 10;
    tbuf[2] = ':'; tbuf[3] = '0' + mins / 10; tbuf[4] = '0' + mins % 10;
    tbuf[5] = '\0';
    int clock_tw = text_width(tbuf);
    draw_text_bold(tray_x + (tray_w - clock_tw) / 2, ty + 11, tbuf, COL_WHITE);
}

/* ====== XP START MENU ====== */
#define SM_X 2
#define SM_W 200
#define SM_ITEM_H 24
#define MENU_COUNT 8
#define SM_BANNER_H 48
#define SM_BOTTOM_H 32
#define SM_BODY_H (MENU_COUNT * SM_ITEM_H + 8)
#define SM_H (SM_BANNER_H + SM_BODY_H + SM_BOTTOM_H)
#define SM_Y (GFX_H - TASKBAR_H - SM_H)

static const char* menu_items[] = {
    "Terminal", "Calculator", "Notepad", "Files", "3D Demo", "About", "---", "Turn Off Computer", NULL
};

/* Icons for menu items (which icon type to draw) */
typedef enum { ICON_TERM, ICON_CALC, ICON_NOTE, ICON_FILES, ICON_3D, ICON_ABOUT, ICON_NONE, ICON_EXIT } menu_icon_t;
static const menu_icon_t menu_icons[] = {
    ICON_TERM, ICON_CALC, ICON_NOTE, ICON_FILES, ICON_3D, ICON_ABOUT, ICON_NONE, ICON_EXIT
};

/* Draw a tiny 12x12 icon for menu items */
static void draw_menu_icon(int x, int y, menu_icon_t icon) {
    switch (icon) {
        case ICON_TERM:
            /* Terminal/console icon */
            fill_rect(x, y, 12, 12, RGB(12, 12, 12));
            draw_rect(x, y, 12, 12, RGB(100, 160, 100));
            /* Prompt '>' */
            draw_char(x + 2, y + 3, '>', RGB(100, 255, 100));
            /* Cursor block */
            fill_rect(x + 7, y + 4, 3, 5, RGB(100, 255, 100));
            break;
        case ICON_CALC:
            fill_rect(x, y, 12, 12, RGB(236, 233, 216));
            draw_rect(x, y, 12, 12, RGB(100, 100, 100));
            /* Tiny calc display */
            fill_rect(x + 2, y + 2, 8, 3, RGB(200, 224, 200));
            /* Buttons grid */
            for (int r = 0; r < 2; r++)
                for (int c = 0; c < 3; c++)
                    fill_rect(x + 2 + c * 3, y + 6 + r * 3, 2, 2, RGB(160, 160, 160));
            break;
        case ICON_NOTE:
            fill_rect(x + 1, y, 10, 12, RGB(255, 255, 240));
            draw_rect(x + 1, y, 10, 12, RGB(100, 100, 160));
            /* Lines of text */
            for (int r = 0; r < 4; r++)
                draw_hline(x + 3, y + 2 + r * 2, 6, RGB(0, 0, 100));
            break;
        case ICON_FILES:
            /* Folder */
            fill_rect(x, y + 2, 12, 10, RGB(255, 220, 80));
            fill_rect(x, y, 6, 3, RGB(255, 200, 50));
            draw_rect(x, y + 2, 12, 10, RGB(180, 140, 30));
            draw_rect(x, y, 6, 3, RGB(180, 140, 30));
            break;
        case ICON_3D:
            /* 3D cube icon */
            fill_rect(x + 2, y + 4, 8, 8, RGB(100, 140, 200));
            fill_rect(x + 4, y + 2, 8, 8, RGB(140, 180, 240));
            draw_rect(x + 4, y + 2, 8, 8, RGB(40, 60, 120));
            /* Connect corners for 3D effect */
            putpixel(x + 3, y + 3, RGB(40, 60, 120));
            break;
        case ICON_ABOUT:
            /* Info circle */
            fill_rect(x + 3, y + 1, 6, 10, RGB(50, 100, 200));
            fill_rect(x + 2, y + 2, 8, 8, RGB(50, 100, 200));
            fill_rect(x + 1, y + 3, 10, 6, RGB(50, 100, 200));
            /* 'i' letter */
            fill_rect(x + 5, y + 3, 2, 1, COL_WHITE);
            fill_rect(x + 5, y + 5, 2, 4, COL_WHITE);
            break;
        case ICON_EXIT:
            /* Power button icon */
            fill_rect(x + 2, y + 2, 8, 8, RGB(200, 40, 40));
            fill_rect(x + 1, y + 3, 10, 6, RGB(200, 40, 40));
            fill_rect(x + 3, y + 1, 6, 10, RGB(200, 40, 40));
            /* Inner circle */
            fill_rect(x + 3, y + 4, 6, 4, RGB(240, 80, 80));
            fill_rect(x + 4, y + 3, 4, 6, RGB(240, 80, 80));
            /* Power line at top */
            fill_rect(x + 5, y + 2, 2, 4, COL_WHITE);
            break;
        default:
            break;
    }
}

static void draw_start_menu(int hover) {
    int x = SM_X, y = SM_Y;

    /* Drop shadow */
    fill_rect_alpha(x + 4, y + 4, SM_W, SM_H, COL_BLACK, 60);
    fill_rect_alpha(x + 3, y + 3, SM_W + 1, SM_H + 1, COL_BLACK, 30);

    /* ---- Top banner (blue gradient with user name) ---- */
    fill_gradient_v(x, y, SM_W, SM_BANNER_H,
                    COL_MENUPANEL_TOP, COL_MENUPANEL_BOT);
    /* Rounded top corners */
    putpixel(x, y, COL_BLACK);
    putpixel(x + 1, y, COL_MENUPANEL_TOP);
    putpixel(x + SM_W - 1, y, COL_BLACK);
    putpixel(x + SM_W - 2, y, COL_MENUPANEL_TOP);

    /* User avatar placeholder (white circle) */
    int ax = x + 10, ay = y + 8;
    fill_rect(ax + 2, ay, 28, 32, RGB(220, 220, 220));
    fill_rect(ax, ay + 2, 32, 28, RGB(220, 220, 220));
    fill_rect(ax + 1, ay + 1, 30, 30, RGB(230, 230, 230));
    draw_rect(ax + 1, ay + 1, 30, 30, RGB(80, 100, 140));
    /* Simple face */
    fill_rect(ax + 10, ay + 8, 12, 8, RGB(200, 170, 140));
    fill_rect(ax + 8, ay + 16, 16, 10, RGB(80, 120, 200));

    /* User name */
    draw_text_bold_shadow(x + 50, y + 18, login_current_user(),
                          COL_WHITE, RGB(0, 20, 80));

    /* ---- Menu body (white) ---- */
    int body_y = y + SM_BANNER_H;
    fill_rect(x, body_y, SM_W, SM_BODY_H, COL_MENUBG);
    /* Blue left border */
    draw_vline(x, body_y, SM_BODY_H, RGB(21, 66, 139));
    /* Right border */
    draw_vline(x + SM_W - 1, body_y, SM_BODY_H, RGB(100, 100, 100));

    /* Menu items */
    for (int i = 0; i < MENU_COUNT; i++) {
        int iy = body_y + 4 + i * SM_ITEM_H;
        if (strcmp(menu_items[i], "---") == 0) {
            /* Separator line */
            draw_hline(x + 8, iy + SM_ITEM_H / 2, SM_W - 16, COL_MENUSEP);
            continue;
        }
        if (i == hover) {
            /* XP selection highlight */
            fill_rect(x + 2, iy, SM_W - 4, SM_ITEM_H, COL_MENUHI);
            draw_menu_icon(x + 10, iy + 6, menu_icons[i]);
            draw_text_bold(x + 30, iy + 8, menu_items[i], COL_MENUHITEXT);
        } else {
            draw_menu_icon(x + 10, iy + 6, menu_icons[i]);
            draw_text(x + 30, iy + 8, menu_items[i], COL_TEXT);
        }
    }

    /* ---- Bottom panel (gray, XP style) ---- */
    int bot_y = body_y + SM_BODY_H;
    fill_gradient_v(x, bot_y, SM_W, SM_BOTTOM_H,
                    RGB(214, 211, 200), RGB(190, 186, 172));
    /* Border */
    draw_hline(x, bot_y, SM_W, RGB(128, 128, 128));
    draw_hline(x, bot_y + SM_BOTTOM_H - 1, SM_W, RGB(100, 100, 100));
    draw_vline(x, bot_y, SM_BOTTOM_H, RGB(100, 100, 100));
    draw_vline(x + SM_W - 1, bot_y, SM_BOTTOM_H, RGB(100, 100, 100));

    /* Bottom corners */
    putpixel(x, bot_y + SM_BOTTOM_H - 1, COL_BLACK);
    putpixel(x + SM_W - 1, bot_y + SM_BOTTOM_H - 1, COL_BLACK);

    /* "Log Off" and "Turn Off" buttons in bottom panel */
    /* These are decorative - the actual "Turn Off" is in the menu above */
    draw_menu_icon(x + SM_W - 30, bot_y + 10, ICON_EXIT);
    draw_text(x + 10, bot_y + 11, "Log Off...", RGB(80, 80, 80));
}

/* ====== RENDER ====== */
void render(void) {
    draw_desktop();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].active) continue;
        draw_window(&windows[i]);
        switch (windows[i].app) {
            case APP_ABOUT:   draw_app_about(&windows[i]); break;
            case APP_CALC:    draw_app_calc(&windows[i]); break;
            case APP_FILES:   draw_app_files(&windows[i]); break;
            case APP_NOTEPAD: draw_app_notepad(&windows[i]); break;
            case APP_IMGVIEW: draw_app_imgview(&windows[i]); break;
            case APP_GL3D:    draw_app_gl3d(&windows[i]); break;
            case APP_TERM:    draw_app_term(&windows[i]); break;
            case APP_ELF_GL:  draw_app_elf_gl(&windows[i]); break;
            default: break;
        }
    }
    draw_taskbar();
    if (start_menu_open) {
        int mx2 = mouse_get_x(), my2 = mouse_get_y();
        int hover = -1;
        int body_y = SM_Y + SM_BANNER_H;
        if (mx2 >= SM_X + 2 && mx2 < SM_X + SM_W - 2 &&
            my2 >= body_y && my2 < body_y + SM_BODY_H) {
            hover = (my2 - body_y - 4) / SM_ITEM_H;
            if (hover >= MENU_COUNT) hover = -1;
            if (hover >= 0 && strcmp(menu_items[hover], "---") == 0) hover = -1;
        }
        draw_start_menu(hover);
    }

    /* Update start button hover state */
    {
        int mx = mouse_get_x(), my = mouse_get_y();
        int ty = GFX_H - TASKBAR_H;
        start_btn_hover = (mx >= 2 && mx < 2 + START_BTN_W &&
                           my >= ty + 3 && my < ty + 3 + START_BTN_H);
    }

    /* Clean up any ELF GL windows whose owning process has died */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].active && windows[i].app == APP_ELF_GL) {
            task_t* owner = task_get_by_pid(windows[i].elf_gl.owner_pid);
            if (!owner || !owner->active || owner->state == TASK_TERMINATED) {
                close_elf_gl_window(&windows[i]);
                close_window(i);
                i--; /* re-check this slot */
            }
        }
    }

    draw_cursor(mouse_get_x(), mouse_get_y());
     static int heart = 0;
    heart++;
    // Draw a small flashing square in the bottom right tray area
    fill_rect(GFX_W - 15, GFX_H - 15, 10, 10, (heart % 20 < 10) ? RGB(255,0,0) : RGB(0,255,0));
    // --- END HEARTBEAT ---

    draw_cursor(mouse_get_x(), mouse_get_y());
    blit();
}

/* ====== EVENT HANDLING ====== */
static void handle_start_click(int mx, int my) {
    int body_y = SM_Y + SM_BANNER_H;
    if (mx >= SM_X + 2 && mx < SM_X + SM_W - 2 &&
        my >= body_y && my < body_y + SM_BODY_H) {
        int item = (my - body_y - 4) / SM_ITEM_H;
        if (item < 0 || item >= MENU_COUNT) { start_menu_open = false; return; }
        if (strcmp(menu_items[item], "---") == 0) return;
        start_menu_open = false;
        if (item == 0) {
            /* Terminal */
            int idx = open_window("Terminal", 60, 30, 520, 380, APP_TERM);
            if (idx >= 0) term_init_window(&windows[idx]);
        } else if (item == 1) {
            int idx = open_window("Calculator", 220, 80, 195, 185, APP_CALC);
            if (idx >= 0) strcpy(windows[idx].calc.display, "0");
        } else if (item == 2) {
            open_window("Untitled - Notepad", 80, 30, 440, 360, APP_NOTEPAD);
        } else if (item == 3) {
            int idx = open_window("My Computer", 150, 50, 280, 340, APP_FILES);
            if (idx >= 0) {
                char hdir[64];
                ksnprintf(hdir, sizeof(hdir), "/home/%s", login_current_user());
                strcpy(windows[idx].files.cwd, hdir);
            }
        } else if (item == 4) {
            int idx = open_window("3D Demo", 100, 60, GL_WIN_W + 14, GL_WIN_H + TITLEBAR_H + 10, APP_GL3D);
            if (idx >= 0) gl3d_init_window(&windows[idx]);
        } else if (item == 5) {
            open_window("About MicroKernel OS", 170, 100, 290, 230, APP_ABOUT);
        } else if (item == 7) {
            gui_running = false;
        }
    } else { start_menu_open = false; }
}

static void handle_mouse(void) {
    int mx = mouse_get_x(), my = mouse_get_y();
    bool lclick = mouse_left_click();
    bool lheld = mouse_left_held();
    if (dragging) {
        if (lheld && drag_idx >= 0 && windows[drag_idx].active) {
            windows[drag_idx].x = mx - drag_ox;
            windows[drag_idx].y = my - drag_oy;
            if (windows[drag_idx].y < 0) windows[drag_idx].y = 0;
            if (windows[drag_idx].y > GFX_H - TASKBAR_H - TITLEBAR_H)
                windows[drag_idx].y = GFX_H - TASKBAR_H - TITLEBAR_H;
            gui_mark_dirty();
        } else { dragging = false; drag_idx = -1; gui_mark_dirty(); }
        return;
    }
    if (!lclick) return;
    gui_mark_dirty();  /* Any click causes a visual change */
    if (start_menu_open) { handle_start_click(mx, my); return; }
    if (my >= GFX_H - TASKBAR_H) {
        if (mx >= 2 && mx < 2 + START_BTN_W && my >= GFX_H - TASKBAR_H + 3) {
            start_menu_open = !start_menu_open; return;
        }
        int bx = START_BTN_W + 12;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (!windows[i].active) continue;
            if (mx >= bx && mx < bx + 136) { bring_to_front(i); return; }
            bx += 139;
        }
        return;
    }
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        window_t* win = &windows[i];
        if (!win->active) continue;
        if (mx < win->x || mx >= win->x + win->w || my < win->y || my >= win->y + win->h) continue;
        int rx = mx - win->x, ry = my - win->y;
        bring_to_front(i);
        int ni = focus_idx; win = &windows[ni];
        /* Close button hit test */
        int cbx = win->w - 27, cby = 6;
        if (rx >= cbx && rx < cbx + 21 && ry >= cby && ry < cby + 16) {
            if (win->app == APP_ELF_GL) close_elf_gl_window(win);
            close_window(ni); return;
        }
        if (ry < TITLEBAR_H + 3) { dragging = true; drag_idx = ni; drag_ox = rx; drag_oy = ry; return; }
        switch (win->app) {
            case APP_ABOUT: click_about(win, rx, ry); break;
            case APP_CALC:  click_calc(win, rx, ry); break;
            case APP_FILES: click_files(win, rx, ry); break;
            case APP_TERM:  click_terminal(win, rx, ry); break;
            default: break;
        }
        return;
    }
    start_menu_open = false;
}

static void handle_keyboard(void) {
    if (!keyboard_haskey()) return;
    gui_mark_dirty();  /* Any keystroke causes a visual change */
    char key = keyboard_getchar();
    if (key == 27) { if (start_menu_open) start_menu_open = false; return; }
    if (focus_idx >= 0 && windows[focus_idx].active) {
        window_t* win = &windows[focus_idx];
        switch (win->app) {
            case APP_NOTEPAD: key_notepad(win, key); break;
            case APP_CALC:
                if (key >= '0' && key <= '9') {
                    if (!win->calc.entered) { win->calc.operand = 0; win->calc.entered = true; }
                    win->calc.operand = win->calc.operand * 10 + (key - '0');
                    calc_update_display(win);
                } else if (key == '+' || key == '-' || key == '*' || key == '/') {
                    if (win->calc.has_op && win->calc.entered) do_calc_op(win);
                    else if (!win->calc.has_op)
                        win->calc.accum = win->calc.entered ? win->calc.operand : win->calc.accum;
                    win->calc.op = key; win->calc.has_op = true; win->calc.entered = false;
                    calc_update_display(win);
                } else if (key == '\n' || key == '=') {
                    if (win->calc.has_op) { do_calc_op(win); win->calc.has_op = false; win->calc.entered = false; }
                    calc_update_display(win);
                }
                break;
            case APP_GL3D: key_gl3d(win, key); break;
            case APP_TERM: key_terminal(win, key); break;
            default: break;
        }
    }
}

/* ====== TEXT MODE RESTORE ====== */
static uint8_t saved_text_buf[80 * 25 * 2];

static void save_text_mode(void) {
    volatile uint16_t* tv = (volatile uint16_t*)0xB8000;
    for (int i = 0; i < 80 * 25; i++) ((uint16_t*)saved_text_buf)[i] = tv[i];
}

static void set_pal(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    outb(0x3C8, idx); outb(0x3C9, r >> 2); outb(0x3C9, g >> 2); outb(0x3C9, b >> 2);
}

static void restore_text_mode(void) {
    bga_disable();
    static const uint8_t m03_misc = 0x67;
    static const uint8_t m03_seq[] = {0x03,0x00,0x03,0x00,0x02};
    static const uint8_t m03_crtc[] = {
        0x5F,0x4F,0x50,0x82,0x55,0x81,0xBF,0x1F,
        0x00,0x4F,0x0D,0x0E,0x00,0x00,0x00,0x50,
        0x9C,0x0E,0x8F,0x28,0x1F,0x96,0xB9,0xA3,0xFF};
    static const uint8_t m03_gc[] = {0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF};
    static const uint8_t m03_ac[] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,
        0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
        0x0C,0x00,0x0F,0x08,0x00};

    outb(0x3C2, m03_misc);
    for (int i = 0; i < 5; i++) { outb(0x3C4, i); outb(0x3C5, m03_seq[i]); }
    outb(0x3D4, 0x03); outb(0x3D5, inb(0x3D5)|0x80);
    outb(0x3D4, 0x11); outb(0x3D5, inb(0x3D5)&~0x80);
    for (int i = 0; i < 25; i++) { outb(0x3D4, i); outb(0x3D5, m03_crtc[i]); }
    for (int i = 0; i < 9; i++) { outb(0x3CE, i); outb(0x3CF, m03_gc[i]); }
    inb(0x3DA);
    for (int i = 0; i < 21; i++) { outb(0x3C0, i); outb(0x3C0, m03_ac[i]); }
    outb(0x3C0, 0x20);

    static const uint8_t tp[][3] = {
        {0,0,0},{0,0,170},{0,170,0},{0,170,170},
        {170,0,0},{170,0,170},{170,85,0},{170,170,170},
        {85,85,85},{85,85,255},{85,255,85},{85,255,255},
        {255,85,85},{255,85,255},{255,255,85},{255,255,255}};
    for (int i = 0; i < 16; i++) set_pal(i, tp[i][0], tp[i][1], tp[i][2]);

    outb(0x3C4,2); outb(0x3C5,0x04); outb(0x3C4,4); outb(0x3C5,0x06);
    outb(0x3CE,4); outb(0x3CF,0x02); outb(0x3CE,5); outb(0x3CF,0x00);
    outb(0x3CE,6); outb(0x3CF,0x00);
    volatile uint8_t* vmem = (volatile uint8_t*)0xA0000;
    for (int c = 0; c < 128; c++) {
        for (int row = 0; row < 16; row++) vmem[c*32+row] = font_data[c][row];
        for (int row = 16; row < 32; row++) vmem[c*32+row] = 0;
    }
    outb(0x3C4,2); outb(0x3C5,0x03); outb(0x3C4,4); outb(0x3C5,0x02);
    outb(0x3CE,4); outb(0x3CF,0x00); outb(0x3CE,5); outb(0x3CF,0x10);
    outb(0x3CE,6); outb(0x3CF,0x0E);

    volatile uint16_t* tv = (volatile uint16_t*)0xB8000;
    for (int i = 0; i < 80 * 25; i++) tv[i] = ((uint16_t*)saved_text_buf)[i];
}

/* ====== RESOLUTION PICKER ====== */
static const struct { uint16_t w, h; const char* label; } res_modes[] = {
    { 640,  480,  " 1)  640 x 480  (VGA)" },
    { 800,  600,  " 2)  800 x 600  (SVGA)" },
    { 1024, 768,  " 3) 1024 x 768  (XGA)  [default]" },
    { 1280, 720,  " 4) 1280 x 720  (HD 720p)" },
    { 1280, 1024, " 5) 1280 x 1024 (SXGA)" },
    { 1600, 900,  " 6) 1600 x 900  (HD+)" },
    { 1920, 1080, " 7) 1920 x 1080 (Full HD)" },
};
#define NUM_RES_MODES 7

/* ====== MAIN ====== */
void gui_start(void) {
    read_vga_font();
    save_text_mode();

    bool have_bga = bga_detect();
    bool have_virtio = false;

    if (!have_bga) {
        /* Try virtio-gpu as fallback */
        have_virtio = virtio_gpu_available();
        if (!have_virtio) {
            kprintf("GUI: No graphics adapter found.\n");
            kprintf("     Need QEMU with -device VGA or -device virtio-gpu-pci\n");
            return;
        }
        kprintf("GUI: Using virtio-gpu backend\n");
    }

    /* Resolution selection */
    kprintf("\n  Select resolution:\n\n");
    for (int i = 0; i < NUM_RES_MODES; i++)
        kprintf("  %s\n", res_modes[i].label);
    kprintf("\n  Choice (1-%d, Enter=3): ", NUM_RES_MODES);

    char ch = keyboard_getchar();
    int choice = 2;
    if (ch >= '1' && ch <= ('0' + NUM_RES_MODES)) {
        choice = ch - '1';
        kprintf("%c\n", ch);
    } else {
        kprintf("3\n");
    }

    GFX_W = res_modes[choice].w;
    GFX_H = res_modes[choice].h;

    kprintf("  Setting %ux%ux32...\n", GFX_W, GFX_H);

    uint32_t fb_pixels = (uint32_t)GFX_W * GFX_H;
    backbuf = (uint32_t*)kmalloc(fb_pixels * sizeof(uint32_t));
    if (!backbuf) {
        kprintf("GUI: Failed to allocate %u KB backbuffer.\n",
                (fb_pixels * 4) / 1024);
        return;
    }

  
        /* VirtIO-GPU backend */
        using_virtio_gpu = true;
        if (!virtio_gpu_set_mode(GFX_W, GFX_H)) {
            kprintf("GUI: virtio-gpu failed to set %ux%ux32 mode.\n", GFX_W, GFX_H);
            kfree(backbuf); backbuf = NULL;
            return;
        }
        /* Point framebuffer at the virtio-gpu FB (for any code that reads it directly) */
        framebuffer = (volatile uint32_t*)virtio_gpu_get_fb();
    

    mouse_init();
    mouse_set_bounds(GFX_W - 1, GFX_H - 1);

    for (uint32_t i = 0; i < fb_pixels; i++) backbuf[i] = COL_DESKTOP_TOP;
    memset(windows, 0, sizeof(windows));
    focus_idx = -1;
    start_menu_open = false;
    gui_running = true;
    dragging = false;
    start_btn_hover = false;
    needs_redraw = true;
    prev_mouse_x = -1;
    prev_mouse_y = -1;

    load_wallpaper();

    while (gui_running) {
        mouse_poll();

        /* Check if mouse moved — if so, must redraw (cursor position changed) */
        {
            int mx = mouse_get_x(), my = mouse_get_y();
            if (mx != prev_mouse_x || my != prev_mouse_y) {
                prev_mouse_x = mx;
                prev_mouse_y = my;
                gui_mark_dirty();
            }
        }

        handle_mouse();
        handle_keyboard();

        /* Active ELF GL windows are continuously animating — always redraw */
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (windows[i].active && windows[i].app == APP_ELF_GL) {
                gui_mark_dirty();
                break;
            }
        }

        /* Only redraw + blit when something actually changed */
        if (needs_redraw) {
            render();
            needs_redraw = false;
        }

        task_sleep(10);
    }

    mouse_set_bounds(319, 199);

    if (using_virtio_gpu) {
        virtio_gpu_disable();
        using_virtio_gpu = false;
    } else {
        restore_text_mode();
    }

    if (gl_inited) { glClose(); gl_inited = false; }
    if (gl_pixbuf) { kfree(gl_pixbuf); gl_pixbuf = NULL; }
    if (wallpaper) { kfree(wallpaper); wallpaper = NULL; wallpaper_loaded = false; }
    if (backbuf) { kfree(backbuf); backbuf = NULL; }


}