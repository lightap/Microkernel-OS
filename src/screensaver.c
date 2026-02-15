#include "screensaver.h"
#include "vga.h"
#include "keyboard.h"
#include "timer.h"

static uint32_t ss_rng = 7919;
static uint32_t ss_rand(void) {
    ss_rng ^= ss_rng << 13;
    ss_rng ^= ss_rng >> 17;
    ss_rng ^= ss_rng << 5;
    return ss_rng;
}

/* ===== MATRIX RAIN ===== */
void screensaver_matrix(void) {
    uint16_t* vga = (uint16_t*)0xB8000;
    int drops[VGA_WIDTH];
    int speeds[VGA_WIDTH];

    ss_rng = timer_get_ticks();
    for (int x = 0; x < VGA_WIDTH; x++) {
        drops[x] = -(int)(ss_rand() % VGA_HEIGHT);
        speeds[x] = 1 + ss_rand() % 3;
    }

    /* Clear screen */
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        vga[i] = 0x0020;

    while (!keyboard_haskey()) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            int y = drops[x];

            /* Draw head (bright green) */
            if (y >= 0 && y < VGA_HEIGHT)
                vga[y * VGA_WIDTH + x] = 0x0A00 | (33 + ss_rand() % 93);

            /* Fade trail */
            if (y - 1 >= 0 && y - 1 < VGA_HEIGHT)
                vga[(y - 1) * VGA_WIDTH + x] = (vga[(y - 1) * VGA_WIDTH + x] & 0x00FF) | 0x0200;

            /* Clear far trail */
            int trail_end = y - 8 - (int)(ss_rand() % 8);
            if (trail_end >= 0 && trail_end < VGA_HEIGHT)
                vga[trail_end * VGA_WIDTH + x] = 0x0020;

            drops[x] += speeds[x];
            if (drops[x] > VGA_HEIGHT + 20) {
                drops[x] = -(int)(ss_rand() % 20);
                speeds[x] = 1 + ss_rand() % 3;
            }
        }
        timer_sleep(50);
    }
    keyboard_getchar(); /* Consume the key */
    terminal_clear();
}

/* ===== STARFIELD ===== */
#define NUM_STARS 80
void screensaver_starfield(void) {
    uint16_t* vga = (uint16_t*)0xB8000;
    int sx[NUM_STARS], sy[NUM_STARS], sz[NUM_STARS];
    ss_rng = timer_get_ticks();

    for (int i = 0; i < NUM_STARS; i++) {
        sx[i] = (ss_rand() % 400) - 200;
        sy[i] = (ss_rand() % 250) - 125;
        sz[i] = ss_rand() % 100 + 1;
    }

    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        vga[i] = 0x0020;

    while (!keyboard_haskey()) {
        /* Clear */
        for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
            vga[i] = 0x0020;

        for (int i = 0; i < NUM_STARS; i++) {
            /* Project 3D to 2D */
            int px = (sx[i] * 100) / sz[i] + VGA_WIDTH / 2;
            int py = (sy[i] * 50) / sz[i] + VGA_HEIGHT / 2;

            if (px >= 0 && px < VGA_WIDTH && py >= 0 && py < VGA_HEIGHT) {
                uint8_t color;
                char ch;
                if (sz[i] < 20)      { color = 0x0F; ch = '@'; }
                else if (sz[i] < 50) { color = 0x07; ch = '*'; }
                else                 { color = 0x08; ch = '.'; }
                vga[py * VGA_WIDTH + px] = ((uint16_t)color << 8) | ch;
            }

            sz[i] -= 2;
            if (sz[i] <= 0) {
                sx[i] = (ss_rand() % 400) - 200;
                sy[i] = (ss_rand() % 250) - 125;
                sz[i] = 100;
            }
        }
        timer_sleep(30);
    }
    keyboard_getchar();
    terminal_clear();
}

/* ===== PIPES ===== */
void screensaver_pipes(void) {
    uint16_t* vga = (uint16_t*)0xB8000;
    ss_rng = timer_get_ticks();

    int x = VGA_WIDTH / 2, y = VGA_HEIGHT / 2;
    int dir = ss_rand() % 4; /* 0=up 1=right 2=down 3=left */
    uint8_t colors[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E };
    uint8_t color = colors[ss_rand() % 12];
    /* Box drawing chars: ─ │ ┌ ┐ └ ┘ */
    char h_pipe = 0xC4, v_pipe = 0xB3;
    char corners[] = { 0xDA, 0xBF, 0xC0, 0xD9 }; /* ┌ ┐ └ ┘ */

    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        vga[i] = 0x0020;

    int steps = 0;
    while (!keyboard_haskey()) {
        /* Draw pipe at current position */
        char ch;
        int new_dir = dir;

        /* Occasionally change direction */
        if (ss_rand() % 5 == 0) {
            new_dir = ss_rand() % 4;
            if (new_dir == (dir + 2) % 4) new_dir = dir; /* Don't reverse */
        }

        if (new_dir == dir) {
            ch = (dir == 0 || dir == 2) ? v_pipe : h_pipe;
        } else {
            /* Corner piece */
            if ((dir == 0 && new_dir == 1) || (dir == 3 && new_dir == 2)) ch = corners[3];
            else if ((dir == 0 && new_dir == 3) || (dir == 1 && new_dir == 2)) ch = corners[2];
            else if ((dir == 2 && new_dir == 1) || (dir == 3 && new_dir == 0)) ch = corners[1];
            else ch = corners[0];
        }

        dir = new_dir;
        vga[y * VGA_WIDTH + x] = ((uint16_t)color << 8) | ch;

        /* Move */
        int dx[] = {0, 1, 0, -1}, dy[] = {-1, 0, 1, 0};
        x += dx[dir]; y += dy[dir];

        if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT) {
            x = ss_rand() % VGA_WIDTH;
            y = ss_rand() % VGA_HEIGHT;
            color = colors[ss_rand() % 12];
        }

        steps++;
        if (steps % 200 == 0) color = colors[ss_rand() % 12];

        timer_sleep(20);
    }
    keyboard_getchar();
    terminal_clear();
}
