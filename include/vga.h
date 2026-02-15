#ifndef VGA_H
#define VGA_H

#include "types.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

enum vga_color {
    VGA_BLACK = 0, VGA_BLUE = 1, VGA_GREEN = 2, VGA_CYAN = 3,
    VGA_RED = 4, VGA_MAGENTA = 5, VGA_BROWN = 6, VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8, VGA_LIGHT_BLUE = 9, VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11, VGA_LIGHT_RED = 12, VGA_LIGHT_MAGENTA = 13,
    VGA_YELLOW = 14, VGA_WHITE = 15,
};

void terminal_init(void);
void terminal_clear(void);
void terminal_setcolor(uint8_t color);
uint8_t terminal_getcolor(void);
void terminal_putchar(char c);
void terminal_print(const char* str);
void terminal_print_hex(uint32_t value);
void terminal_print_dec(uint32_t value);
void terminal_print_dec64(uint64_t value);
void terminal_print_colored(const char* str, uint8_t color);
void terminal_backspace(void);
void terminal_set_cursor(int row, int col);
void terminal_get_cursor(int* row, int* col);
void terminal_print_at(int row, int col, const char* str, uint8_t color);
void terminal_draw_box(int row, int col, int w, int h, uint8_t color);

/* Output hook for pipes/redirection */
typedef void (*terminal_hook_t)(char c);
void terminal_set_hook(terminal_hook_t hook);

/* Printf-lite: supports %s %d %x %c %% */
void kprintf(const char* fmt, ...);

#endif
