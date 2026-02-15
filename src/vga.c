#include "vga.h"

#define VGA_MEMORY 0xB8000

static uint16_t* const vga_buffer = (uint16_t*)VGA_MEMORY;
static uint8_t  term_row;
static uint8_t  term_col;
static uint8_t  term_color;
static terminal_hook_t output_hook = NULL;

void terminal_set_hook(terminal_hook_t hook) { output_hook = hook; }

static inline uint16_t vga_entry(unsigned char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

static void scroll(void) {
    for (int y = 0; y < VGA_HEIGHT - 1; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            vga_buffer[y * VGA_WIDTH + x] = vga_buffer[(y + 1) * VGA_WIDTH + x];
    for (int x = 0; x < VGA_WIDTH; x++)
        vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', term_color);
    term_row = VGA_HEIGHT - 1;
}

static void update_cursor(void) {
    uint16_t pos = term_row * VGA_WIDTH + term_col;
    outb(0x3D4, 14);
    outb(0x3D5, (uint8_t)(pos >> 8));
    outb(0x3D4, 15);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
}

void terminal_init(void) {
    term_row = 0;
    term_col = 0;
    term_color = VGA_LIGHT_GREY | (VGA_BLACK << 4);
    terminal_clear();
}

void terminal_clear(void) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        vga_buffer[i] = vga_entry(' ', term_color);
    term_row = 0;
    term_col = 0;
    update_cursor();
}

void terminal_setcolor(uint8_t color) { term_color = color; }
uint8_t terminal_getcolor(void) { return term_color; }

void terminal_putchar(char c) {
    if (output_hook) output_hook(c);
    if (c == '\n') { term_col = 0; term_row++; }
    else if (c == '\t') { term_col = (term_col + 8) & ~7; }
    else if (c == '\r') { term_col = 0; }
    else {
        vga_buffer[term_row * VGA_WIDTH + term_col] = vga_entry(c, term_color);
        term_col++;
    }
    if (term_col >= VGA_WIDTH) { term_col = 0; term_row++; }
    if (term_row >= VGA_HEIGHT) scroll();
    update_cursor();
}

void terminal_backspace(void) {
    if (term_col > 0) {
        term_col--;
    } else if (term_row > 0) {
        term_row--;
        term_col = VGA_WIDTH - 1;
    }
    vga_buffer[term_row * VGA_WIDTH + term_col] = vga_entry(' ', term_color);
    update_cursor();
}

void terminal_print(const char* str) {
    while (*str) terminal_putchar(*str++);
}

void terminal_print_hex(uint32_t value) {
    const char hex[] = "0123456789ABCDEF";
    terminal_print("0x");
    for (int i = 28; i >= 0; i -= 4)
        terminal_putchar(hex[(value >> i) & 0xF]);
}

void terminal_print_dec(uint32_t value) {
    if (value == 0) { terminal_putchar('0'); return; }
    char buf[12];
    int i = 0;
    while (value > 0) { buf[i++] = '0' + (value % 10); value /= 10; }
    while (--i >= 0) terminal_putchar(buf[i]);
}

void terminal_print_dec64(uint64_t value) {
    if (value == 0) { terminal_putchar('0'); return; }
    char buf[21];
    int i = 0;
    while (value > 0) { buf[i++] = '0' + (value % 10); value /= 10; }
    while (--i >= 0) terminal_putchar(buf[i]);
}

void terminal_print_colored(const char* str, uint8_t color) {
    uint8_t old = term_color;
    term_color = color;
    terminal_print(str);
    term_color = old;
}

void terminal_set_cursor(int row, int col) {
    term_row = row;
    term_col = col;
    update_cursor();
}

void terminal_get_cursor(int* row, int* col) {
    *row = term_row;
    *col = term_col;
}

void terminal_print_at(int row, int col, const char* str, uint8_t color) {
    uint8_t old_color = term_color;
    int old_row = term_row, old_col = term_col;
    term_row = row; term_col = col; term_color = color;
    terminal_print(str);
    term_color = old_color;
    term_row = old_row;
    term_col = old_col;
    update_cursor();
}

void terminal_draw_box(int row, int col, int w, int h, uint8_t color) {
    uint8_t old = term_color;
    term_color = color;
    /* Corners and edges using CP437 box drawing */
    vga_buffer[row * VGA_WIDTH + col] = vga_entry(0xC9, color);
    vga_buffer[row * VGA_WIDTH + col + w - 1] = vga_entry(0xBB, color);
    vga_buffer[(row + h - 1) * VGA_WIDTH + col] = vga_entry(0xC8, color);
    vga_buffer[(row + h - 1) * VGA_WIDTH + col + w - 1] = vga_entry(0xBC, color);
    for (int x = 1; x < w - 1; x++) {
        vga_buffer[row * VGA_WIDTH + col + x] = vga_entry(0xCD, color);
        vga_buffer[(row + h - 1) * VGA_WIDTH + col + x] = vga_entry(0xCD, color);
    }
    for (int y = 1; y < h - 1; y++) {
        vga_buffer[(row + y) * VGA_WIDTH + col] = vga_entry(0xBA, color);
        vga_buffer[(row + y) * VGA_WIDTH + col + w - 1] = vga_entry(0xBA, color);
        for (int x = 1; x < w - 1; x++)
            vga_buffer[(row + y) * VGA_WIDTH + col + x] = vga_entry(' ', color);
    }
    term_color = old;
}

/* Variadic kprintf - supports %s %d %u %x %c %% */
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)

void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 's': terminal_print(va_arg(args, const char*)); break;
                case 'd': {
                    int v = va_arg(args, int);
                    if (v < 0) { terminal_putchar('-'); v = -v; }
                    terminal_print_dec((uint32_t)v);
                    break;
                }
                case 'u': terminal_print_dec(va_arg(args, uint32_t)); break;
                case 'x': terminal_print_hex(va_arg(args, uint32_t)); break;
                case 'c': terminal_putchar((char)va_arg(args, int)); break;
                case '%': terminal_putchar('%'); break;
                default: terminal_putchar('%'); terminal_putchar(*fmt); break;
            }
        } else {
            terminal_putchar(*fmt);
        }
        fmt++;
    }

    va_end(args);
}
