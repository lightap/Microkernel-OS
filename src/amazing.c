#include "userlib.h"

/*
 * amazing.c – ELF user-space program that creates a stunning
 * fireworks display using particle physics simulation.
 *
 * Usage: exec amazing.elf
 *
 * Features:
 * - Multiple firework types (burst, fountain, sparkler)
 * - Realistic physics with gravity and air resistance
 * - Trail effects and particle fading
 * - Dynamic color palettes
 * - Interactive statistics display
 */

/* ---- Framebuffer ---- */
static uint32_t* fb;
#define FB_W  ELF_GUI_FB_W   /* 320 */
#define FB_H  ELF_GUI_FB_H   /* 200 */

#define RGB(r,g,b) ((uint32_t)(((r)<<16)|((g)<<8)|(b)))

/* ---- Particle System ---- */
#define MAX_PARTICLES 1200

typedef struct {
    float x, y;           /* position */
    float vx, vy;         /* velocity */
    float life;           /* 1.0 = full, 0.0 = dead */
    float fade;           /* fade rate */
    uint32_t color;       /* base color */
    uint8_t type;         /* 0=burst, 1=trail, 2=sparkle */
    uint8_t active;
} particle_t;

static particle_t particles[MAX_PARTICLES];
static uint32_t active_count = 0;
static uint32_t total_spawned = 0;

/* ---- Firework System ---- */
#define MAX_FIREWORKS 8

typedef struct {
    float x, y;
    float vx, vy;
    uint32_t color;
    uint8_t type;         /* 0=normal, 1=cascade, 2=ring, 3=fountain */
    uint8_t exploded;
    float fuse;
} firework_t;

static firework_t fireworks[MAX_FIREWORKS];
static uint32_t next_launch = 0;
static uint32_t fireworks_launched = 0;

/* ---- Random number generator ---- */
static uint32_t rand_state = 12345;

static uint32_t rand_next(void) {
    rand_state = rand_state * 1103515245 + 12345;
    return (rand_state / 65536) % 32768;
}

static float randf(void) {
    return (float)rand_next() / 32768.0f;
}

/* ---- Color palettes ---- */
static const uint32_t palette_hot[] = {
    RGB(255, 50, 0), RGB(255, 120, 0), RGB(255, 200, 50), RGB(255, 255, 100)
};
static const uint32_t palette_cool[] = {
    RGB(0, 100, 255), RGB(0, 200, 255), RGB(100, 255, 255), RGB(200, 200, 255)
};
static const uint32_t palette_green[] = {
    RGB(0, 255, 50), RGB(50, 255, 100), RGB(100, 255, 150), RGB(150, 255, 200)
};
static const uint32_t palette_purple[] = {
    RGB(200, 0, 255), RGB(255, 0, 200), RGB(255, 100, 255), RGB(255, 150, 255)
};
static const uint32_t palette_rainbow[] = {
    RGB(255, 0, 0), RGB(255, 127, 0), RGB(255, 255, 0), RGB(0, 255, 0),
    RGB(0, 0, 255), RGB(75, 0, 130), RGB(148, 0, 211)
};

/* ---- Drawing functions ---- */
static inline void putpx(int x, int y, uint32_t col) {
    if ((unsigned)x < FB_W && (unsigned)y < FB_H)
        fb[y * FB_W + x] = col;
}

static void draw_particle(int x, int y, uint32_t color, float life) {
    if (life <= 0.0f) return;
    
    /* Fade color based on life */
    uint8_t r = ((color >> 16) & 0xFF) * life;
    uint8_t g = ((color >> 8) & 0xFF) * life;
    uint8_t b = (color & 0xFF) * life;
    uint32_t faded = RGB(r, g, b);
    
    /* Draw 2x2 particle with bloom */
    putpx(x, y, faded);
    putpx(x+1, y, faded);
    putpx(x, y+1, faded);
    putpx(x+1, y+1, faded);
    
    /* Bloom effect for bright particles */
    if (life > 0.7f) {
        uint32_t bloom = RGB(r/2, g/2, b/2);
        putpx(x-1, y, bloom);
        putpx(x+2, y, bloom);
        putpx(x, y-1, bloom);
        putpx(x, y+2, bloom);
    }
}

/* ---- Font (simplified from hello.c) ---- */
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

/* ---- Particle management ---- */
static void spawn_particle(float x, float y, float vx, float vy,
                           uint32_t color, uint8_t type, float life_span) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!particles[i].active) {
            particles[i].x = x;
            particles[i].y = y;
            particles[i].vx = vx;
            particles[i].vy = vy;
            particles[i].life = 1.0f;
            particles[i].fade = 1.0f / life_span;
            particles[i].color = color;
            particles[i].type = type;
            particles[i].active = 1;
            active_count++;
            total_spawned++;
            return;
        }
    }
}

static void update_particles(float dt) {
    const float gravity = 60.0f;
    const float air_resistance = 0.98f;
    
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!particles[i].active) continue;
        
        particle_t* p = &particles[i];
        
        /* Physics */
        p->vy += gravity * dt;
        p->vx *= air_resistance;
        p->vy *= air_resistance;
        
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        
        /* Life */
        p->life -= p->fade * dt;
        
        if (p->life <= 0.0f || p->y > FB_H + 10) {
            p->active = 0;
            active_count--;
        }
    }
}

static void draw_particles(void) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!particles[i].active) continue;
        particle_t* p = &particles[i];
        draw_particle((int)p->x, (int)p->y, p->color, p->life);
    }
}

/* ---- Firework management ---- */
static uint32_t get_random_color(void) {
    uint32_t choice = rand_next() % 5;
    switch (choice) {
        case 0: return palette_hot[rand_next() % 4];
        case 1: return palette_cool[rand_next() % 4];
        case 2: return palette_green[rand_next() % 4];
        case 3: return palette_purple[rand_next() % 4];
        default: return palette_rainbow[rand_next() % 7];
    }
}

static void launch_firework(void) {
    for (int i = 0; i < MAX_FIREWORKS; i++) {
        if (fireworks[i].exploded) {  // <-- FIXED: Just check if slot is available
            fireworks[i].x = 40 + randf() * (FB_W - 80);
            fireworks[i].y = FB_H - 5;
            fireworks[i].vx = (randf() - 0.5f) * 20.0f;
            fireworks[i].vy = -80.0f - randf() * 40.0f;
            fireworks[i].color = get_random_color();
            fireworks[i].type = rand_next() % 4;
            fireworks[i].exploded = 0;  // Mark as active
            fireworks[i].fuse = 0.8f + randf() * 0.6f;
            fireworks_launched++;
            return;
        }
    }
}
static void explode_firework(firework_t* fw) {
    const int burst_count[] = { 50, 80, 60, 40 };
    int count = burst_count[fw->type];
    
    switch (fw->type) {
        case 0: /* Normal burst */
            for (int i = 0; i < count; i++) {
                float angle = (float)i / count * 6.28318f;
                float speed = 30.0f + randf() * 30.0f;
                float vx = u_cos(angle) * speed;
                float vy = u_sin(angle) * speed;
                spawn_particle(fw->x, fw->y, fw->vx + vx, fw->vy + vy,
                              fw->color, 0, 60.0f + randf() * 40.0f);
            }
            break;
            
        case 1: /* Cascade */
            for (int i = 0; i < count; i++) {
                float angle = (float)i / count * 6.28318f;
                float speed = 40.0f + randf() * 20.0f;
                float vx = u_cos(angle) * speed;
                float vy = u_sin(angle) * speed - 20.0f;
                uint32_t col = (i % 2) ? fw->color : RGB(255, 255, 100);
                spawn_particle(fw->x, fw->y, fw->vx + vx, fw->vy + vy,
                              col, 0, 80.0f + randf() * 30.0f);
            }
            break;
            
        case 2: /* Ring */
            for (int i = 0; i < count; i++) {
                float angle = (float)i / count * 6.28318f;
                float speed = 45.0f;
                float vx = u_cos(angle) * speed;
                float vy = u_sin(angle) * speed;
                spawn_particle(fw->x, fw->y, fw->vx + vx, fw->vy + vy,
                              fw->color, 2, 50.0f + randf() * 20.0f);
            }
            break;
            
        case 3: /* Fountain */
            for (int i = 0; i < count; i++) {
                float angle = -1.57f + (randf() - 0.5f) * 1.0f;
                float speed = 50.0f + randf() * 30.0f;
                float vx = u_cos(angle) * speed;
                float vy = u_sin(angle) * speed;
                spawn_particle(fw->x, fw->y, fw->vx + vx, fw->vy + vy,
                              fw->color, 1, 100.0f + randf() * 50.0f);
            }
            break;
    }
    
    fw->exploded = 1;
    fw->fuse = 0.0f;
}

static void update_fireworks(float dt) {
    const float gravity = 50.0f;
    
    for (int i = 0; i < MAX_FIREWORKS; i++) {
        firework_t* fw = &fireworks[i];
        if (fw->exploded || fw->fuse <= 0.0f) continue;
        
        /* Physics */
        fw->vy += gravity * dt;
        fw->x += fw->vx * dt;
        fw->y += fw->vy * dt;
        fw->fuse -= dt;
        
        /* Draw rocket trail */
        spawn_particle(fw->x, fw->y, fw->vx * 0.3f, fw->vy * 0.3f,
                      RGB(255, 200, 100), 1, 15.0f);
        
        /* Explode when fuse runs out or apex reached */
        if (fw->fuse <= 0.0f || fw->vy > 0.0f) {
            explode_firework(fw);
        }
    }
}

static void draw_fireworks(void) {
    for (int i = 0; i < MAX_FIREWORKS; i++) {
        firework_t* fw = &fireworks[i];
        if (fw->exploded || fw->fuse <= 0.0f) continue;
        
        int x = (int)fw->x;
        int y = (int)fw->y;
        
        /* Draw bright rocket */
        putpx(x, y, RGB(255, 255, 200));
        putpx(x+1, y, RGB(255, 255, 200));
        putpx(x, y+1, RGB(255, 255, 200));
        putpx(x+1, y+1, RGB(255, 255, 200));
    }
}

int main(void) {
    sys_debug_log("amazing.elf: requesting GUI window...\n");

    uint32_t fb_addr = sys_gui_win_open("amazing.elf - Fireworks!");
    if (!fb_addr) {
        sys_debug_log("amazing.elf: failed to open GUI window!\n");
        return 1;
    }
    fb = (uint32_t*)fb_addr;
    sys_debug_log("amazing.elf: GUI window opened, starting fireworks show...\n");

    /* Initialize */
    rand_state = sys_gui_get_ticks();
    for (int i = 0; i < MAX_PARTICLES; i++)
        particles[i].active = 0;
    for (int i = 0; i < MAX_FIREWORKS; i++) {
        fireworks[i].exploded = 1;
        fireworks[i].fuse = 0.0f;
    }

    uint32_t frame = 0;
    float time_accum = 0.0f;

    while (1) {
        const float dt = 0.016f; /* ~60 FPS */
        time_accum += dt;

        /* Fade background (motion blur effect) */
        for (int i = 0; i < FB_W * FB_H; i++) {
            uint32_t c = fb[i];
            uint8_t r = ((c >> 16) & 0xFF);
            uint8_t g = ((c >> 8) & 0xFF);
            uint8_t b = (c & 0xFF);
            r = r > 8 ? r - 8 : 0;
            g = g > 8 ? g - 8 : 0;
            b = b > 10 ? b - 10 : 0;
            fb[i] = RGB(r, g, b);
        }

        /* Launch new fireworks */
        if (time_accum >= next_launch) {
            launch_firework();
            next_launch = randf() * 1.5f + 0.3f;
            time_accum = 0.0f;
        }

        /* Update and draw */
        update_fireworks(dt);
        update_particles(dt);
        draw_particles();
        draw_fireworks();

        /* HUD */
        draw_str(4, 4, "AMAZING.ELF", RGB(255, 180, 0));
        draw_str(4, 14, "Fireworks Simulator", RGB(200, 150, 0));
        
        {
            char buf[64];
            /* Active particles */
            strcpy(buf, "Particles: ");
            uint32_t v = active_count;
            char tmp[12]; int ti = 0;
            if (v == 0) tmp[ti++] = '0';
            else while (v > 0) { tmp[ti++] = '0' + (v%10); v /= 10; }
            int p = 11;
            while (ti > 0) buf[p++] = tmp[--ti];
            buf[p] = '\0';
            draw_str(4, FB_H - 20, buf, RGB(100, 200, 255));
            
            /* Fireworks launched */
            strcpy(buf, "Launched: ");
            v = fireworks_launched;
            ti = 0;
            if (v == 0) tmp[ti++] = '0';
            else while (v > 0) { tmp[ti++] = '0' + (v%10); v /= 10; }
            p = 10;
            while (ti > 0) buf[p++] = tmp[--ti];
            buf[p] = '\0';
            draw_str(4, FB_H - 10, buf, RGB(100, 255, 200));
        }

        /* Present frame */
        sys_gui_present();
        sys_sleep(16); /* ~60 FPS */
        frame++;
    }

    return 0;
}
