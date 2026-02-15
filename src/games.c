#include "games.h"
#include "vga.h"
#include "keyboard.h"
#include "timer.h"

/* ========== SNAKE ========== */
#define SNAKE_W 40
#define SNAKE_H 20
#define SNAKE_MAX 200
#define SNAKE_OX 20
#define SNAKE_OY 2

static int snake_x[SNAKE_MAX], snake_y[SNAKE_MAX];
static int snake_len, snake_dir; /* 0=up 1=right 2=down 3=left */
static int food_x, food_y, snake_score;
static bool snake_alive;

static uint32_t rng_state = 12345;
static uint32_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void snake_place_food(void) {
    do {
        food_x = rng() % SNAKE_W;
        food_y = rng() % SNAKE_H;
        bool on_snake = false;
        for (int i = 0; i < snake_len; i++)
            if (snake_x[i] == food_x && snake_y[i] == food_y) { on_snake = true; break; }
        if (!on_snake) break;
    } while (1);
}

static void snake_draw(void) {
    uint16_t* vga = (uint16_t*)0xB8000;

    /* Border */
    for (int x = 0; x <= SNAKE_W + 1; x++) {
        vga[(SNAKE_OY - 1) * 80 + SNAKE_OX + x - 1] = 0x0800 | '#';
        vga[(SNAKE_OY + SNAKE_H) * 80 + SNAKE_OX + x - 1] = 0x0800 | '#';
    }
    for (int y = 0; y < SNAKE_H; y++) {
        vga[(SNAKE_OY + y) * 80 + SNAKE_OX - 1] = 0x0800 | '#';
        vga[(SNAKE_OY + y) * 80 + SNAKE_OX + SNAKE_W] = 0x0800 | '#';
    }

    /* Clear field */
    for (int y = 0; y < SNAKE_H; y++)
        for (int x = 0; x < SNAKE_W; x++)
            vga[(SNAKE_OY + y) * 80 + SNAKE_OX + x] = 0x0000 | ' ';

    /* Food */
    vga[(SNAKE_OY + food_y) * 80 + SNAKE_OX + food_x] = 0x0C00 | '*';

    /* Snake body */
    for (int i = 1; i < snake_len; i++)
        vga[(SNAKE_OY + snake_y[i]) * 80 + SNAKE_OX + snake_x[i]] = 0x0200 | 'o';

    /* Snake head */
    vga[(SNAKE_OY + snake_y[0]) * 80 + SNAKE_OX + snake_x[0]] = 0x0A00 | '@';

    /* Score */
    terminal_set_cursor(SNAKE_OY + SNAKE_H + 1, SNAKE_OX);
    kprintf("Score: %d  |  WASD/Arrows to move, Q to quit    ", snake_score);
}

void game_snake(void) {
    terminal_clear();
    rng_state = timer_get_ticks();

    snake_len = 3;
    snake_dir = 1;
    snake_score = 0;
    snake_alive = true;

    for (int i = 0; i < snake_len; i++) {
        snake_x[i] = SNAKE_W / 2 - i;
        snake_y[i] = SNAKE_H / 2;
    }
    snake_place_food();

    terminal_print_colored("  SNAKE - eat * to grow. Don't hit walls or yourself!\n", 0x0E);

    while (snake_alive) {
        snake_draw();

        /* Wait with input polling */
        uint32_t start = timer_get_ticks();
        int delay = 8 - snake_score / 5;
        if (delay < 2) delay = 2;

        while (timer_get_ticks() - start < (uint32_t)delay) {
            if (keyboard_haskey()) {
                char c = keyboard_getchar();
                if (c == 'w' || c == 'W' || c == (char)KEY_UP)    { if (snake_dir != 2) snake_dir = 0; }
                if (c == 'd' || c == 'D' || c == (char)KEY_RIGHT) { if (snake_dir != 3) snake_dir = 1; }
                if (c == 's' || c == 'S' || c == (char)KEY_DOWN)  { if (snake_dir != 0) snake_dir = 2; }
                if (c == 'a' || c == 'A' || c == (char)KEY_LEFT)  { if (snake_dir != 1) snake_dir = 3; }
                if (c == 'q' || c == 'Q') { terminal_clear(); return; }
            }
            hlt();
        }

        /* Move */
        int nx = snake_x[0], ny = snake_y[0];
        if (snake_dir == 0) ny--;
        if (snake_dir == 1) nx++;
        if (snake_dir == 2) ny++;
        if (snake_dir == 3) nx--;

        /* Wall collision */
        if (nx < 0 || nx >= SNAKE_W || ny < 0 || ny >= SNAKE_H) {
            snake_alive = false; break;
        }

        /* Self collision */
        for (int i = 0; i < snake_len; i++)
            if (snake_x[i] == nx && snake_y[i] == ny) { snake_alive = false; break; }
        if (!snake_alive) break;

        /* Eat food? */
        bool ate = (nx == food_x && ny == food_y);
        if (ate) {
            snake_score++;
            if (snake_len < SNAKE_MAX) snake_len++;
            snake_place_food();
        }

        /* Shift body */
        for (int i = snake_len - 1; i > 0; i--) {
            snake_x[i] = snake_x[i - 1];
            snake_y[i] = snake_y[i - 1];
        }
        snake_x[0] = nx;
        snake_y[0] = ny;
    }

    snake_draw();
    terminal_set_cursor(SNAKE_OY + SNAKE_H / 2, SNAKE_OX + SNAKE_W / 2 - 5);
    terminal_print_colored(" GAME OVER! ", 0x4F);
    terminal_set_cursor(SNAKE_OY + SNAKE_H + 2, SNAKE_OX);
    kprintf("Final score: %d. Press any key...", snake_score);
    keyboard_getchar();
    terminal_clear();
}

/* ========== 2048 ========== */
#define G2048_SIZE 4

static uint32_t board[G2048_SIZE][G2048_SIZE];
static uint32_t score_2048;
static bool game_over_2048;

static void g2048_add_tile(void) {
    int empty[16][2], count = 0;
    for (int y = 0; y < G2048_SIZE; y++)
        for (int x = 0; x < G2048_SIZE; x++)
            if (board[y][x] == 0) { empty[count][0] = y; empty[count][1] = x; count++; }
    if (count == 0) return;
    int idx = rng() % count;
    board[empty[idx][0]][empty[idx][1]] = (rng() % 10 < 9) ? 2 : 4;
}

static uint8_t tile_color(uint32_t val) {
    switch (val) {
        case 2:    return 0x0F;
        case 4:    return 0x0E;
        case 8:    return 0x06;
        case 16:   return 0x0C;
        case 32:   return 0x04;
        case 64:   return 0x04;
        case 128:  return 0x0B;
        case 256:  return 0x03;
        case 512:  return 0x0D;
        case 1024: return 0x05;
        case 2048: return 0x0A;
        default:   return 0x09;
    }
}

static void g2048_draw(void) {
    terminal_set_cursor(2, 25);
    terminal_print_colored("  2048  ", 0x0E);
    terminal_set_cursor(3, 25);
    kprintf("Score: %u", score_2048);

    for (int y = 0; y < G2048_SIZE; y++) {
        terminal_set_cursor(5 + y * 2, 25);
        kprintf("+------+------+------+------+");
        terminal_set_cursor(6 + y * 2, 25);
        for (int x = 0; x < G2048_SIZE; x++) {
            kprintf("|");
            if (board[y][x]) {
                terminal_setcolor(tile_color(board[y][x]));
                if (board[y][x] < 10)        kprintf("  %u   ", board[y][x]);
                else if (board[y][x] < 100)   kprintf("  %u  ", board[y][x]);
                else if (board[y][x] < 1000)  kprintf(" %u  ", board[y][x]);
                else                           kprintf(" %u ", board[y][x]);
                terminal_setcolor(0x07);
            } else {
                kprintf("      ");
            }
        }
        kprintf("|");
    }
    terminal_set_cursor(5 + G2048_SIZE * 2, 25);
    kprintf("+------+------+------+------+");

    terminal_set_cursor(5 + G2048_SIZE * 2 + 2, 25);
    kprintf("WASD/Arrows to move, Q to quit");
}

static bool g2048_slide(int dx, int dy) {
    bool moved = false;
    bool merged[4][4] = {{0}};

    int start_y = (dy > 0) ? G2048_SIZE - 1 : 0;
    int end_y = (dy > 0) ? -1 : G2048_SIZE;
    int step_y = (dy > 0) ? -1 : 1;
    int start_x = (dx > 0) ? G2048_SIZE - 1 : 0;
    int end_x = (dx > 0) ? -1 : G2048_SIZE;
    int step_x = (dx > 0) ? -1 : 1;

    for (int y = start_y; y != end_y; y += step_y) {
        for (int x = start_x; x != end_x; x += step_x) {
            if (board[y][x] == 0) continue;
            int ny = y, nx = x;
            while (1) {
                int ty = ny + dy, tx = nx + dx;
                if (ty < 0 || ty >= G2048_SIZE || tx < 0 || tx >= G2048_SIZE) break;
                if (board[ty][tx] == 0) { ny = ty; nx = tx; }
                else if (board[ty][tx] == board[y][x] && !merged[ty][tx]) {
                    ny = ty; nx = tx; break;
                } else break;
            }
            if (ny != y || nx != x) {
                if (board[ny][nx] == board[y][x]) {
                    board[ny][nx] *= 2;
                    score_2048 += board[ny][nx];
                    merged[ny][nx] = true;
                } else {
                    board[ny][nx] = board[y][x];
                }
                board[y][x] = 0;
                moved = true;
            }
        }
    }
    return moved;
}

static bool g2048_can_move(void) {
    for (int y = 0; y < G2048_SIZE; y++)
        for (int x = 0; x < G2048_SIZE; x++) {
            if (board[y][x] == 0) return true;
            if (x < G2048_SIZE - 1 && board[y][x] == board[y][x + 1]) return true;
            if (y < G2048_SIZE - 1 && board[y][x] == board[y + 1][x]) return true;
        }
    return false;
}

void game_2048(void) {
    terminal_clear();
    rng_state = timer_get_ticks();
    memset(board, 0, sizeof(board));
    score_2048 = 0;
    game_over_2048 = false;

    g2048_add_tile();
    g2048_add_tile();

    while (!game_over_2048) {
        g2048_draw();

        char c = keyboard_getchar();
        bool moved = false;

        if (c == 'w' || c == 'W' || c == (char)KEY_UP)    moved = g2048_slide(0, -1);
        if (c == 's' || c == 'S' || c == (char)KEY_DOWN)   moved = g2048_slide(0, 1);
        if (c == 'a' || c == 'A' || c == (char)KEY_LEFT)   moved = g2048_slide(-1, 0);
        if (c == 'd' || c == 'D' || c == (char)KEY_RIGHT)  moved = g2048_slide(1, 0);
        if (c == 'q' || c == 'Q') break;

        if (moved) g2048_add_tile();
        if (!g2048_can_move()) game_over_2048 = true;
    }

    if (game_over_2048) {
        g2048_draw();
        terminal_set_cursor(12, 30);
        terminal_print_colored(" GAME OVER! ", 0x4F);
        terminal_set_cursor(14, 25);
        kprintf("Final score: %u. Press any key...", score_2048);
        keyboard_getchar();
    }
    terminal_clear();
}
