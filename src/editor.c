#include "editor.h"
#include "vga.h"
#include "keyboard.h"
#include "ramfs.h"

static char lines[EDITOR_MAX_LINES][EDITOR_LINE_LEN];
static int num_lines;
static int cursor_row, cursor_col;
static int scroll_offset;
static char edit_filename[64];
static bool modified;

#define EDIT_ROWS (VGA_HEIGHT - 2) /* Reserve top and bottom rows */
#define STATUS_ROW 0
#define TEXT_START 1

static void editor_draw_status(void) {
    /* Top status bar */
    uint8_t old = terminal_getcolor();
    terminal_set_cursor(STATUS_ROW, 0);
    terminal_setcolor(0x70); /* Black on grey */
    kprintf(" EDIT: %-20s  Ln:%-4d Col:%-4d  %s",
            edit_filename, cursor_row + 1, cursor_col + 1,
            modified ? "[Modified]" : "          ");
    /* Pad to full width */
    int r, c;
    terminal_get_cursor(&r, &c);
    while (c < VGA_WIDTH) { terminal_putchar(' '); c++; }

    /* Bottom help bar */
    terminal_set_cursor(VGA_HEIGHT - 1, 0);
    terminal_setcolor(0x70);
    kprintf(" ^S Save  ^Q Quit  ^G Goto  ^K Cut Line  ^U Paste             ");
    terminal_get_cursor(&r, &c);
    while (c < VGA_WIDTH) { terminal_putchar(' '); c++; }

    terminal_setcolor(old);
}

static void editor_draw_text(void) {
    uint8_t old = terminal_getcolor();
    for (int y = 0; y < EDIT_ROWS; y++) {
        terminal_set_cursor(TEXT_START + y, 0);
        int line_idx = scroll_offset + y;
        if (line_idx < num_lines) {
            terminal_setcolor(0x08);
            /* Line number (3 chars + space) */
            int ln = line_idx + 1;
            if (ln < 10) kprintf("  %d ", ln);
            else if (ln < 100) kprintf(" %d ", ln);
            else kprintf("%d ", ln);
            terminal_setcolor(0x07);
            /* Line content */
            int len = strlen(lines[line_idx]);
            for (int x = 0; x < VGA_WIDTH - 4 && x < len; x++)
                terminal_putchar(lines[line_idx][x]);
            /* Clear rest of line */
            int r, c;
            terminal_get_cursor(&r, &c);
            while (c < VGA_WIDTH) { terminal_putchar(' '); c++; }
        } else {
            terminal_setcolor(0x08);
            kprintf("  ~ ");
            terminal_setcolor(0x07);
            int r, c;
            terminal_get_cursor(&r, &c);
            while (c < VGA_WIDTH) { terminal_putchar(' '); c++; }
        }
    }
    terminal_setcolor(old);
}

static void editor_update_cursor(void) {
    /* Ensure cursor is visible */
    if (cursor_row < scroll_offset)
        scroll_offset = cursor_row;
    if (cursor_row >= scroll_offset + EDIT_ROWS)
        scroll_offset = cursor_row - EDIT_ROWS + 1;

    terminal_set_cursor(TEXT_START + (cursor_row - scroll_offset), 4 + cursor_col);
}

static void editor_insert_char(char c) {
    if (cursor_row >= EDITOR_MAX_LINES) return;
    int len = strlen(lines[cursor_row]);
    if (len >= EDITOR_LINE_LEN - 2) return;

    /* Shift right */
    for (int i = len + 1; i > cursor_col; i--)
        lines[cursor_row][i] = lines[cursor_row][i - 1];
    lines[cursor_row][cursor_col] = c;
    cursor_col++;
    modified = true;
}

static void editor_backspace(void) {
    if (cursor_col > 0) {
        int len = strlen(lines[cursor_row]);
        for (int i = cursor_col - 1; i < len; i++)
            lines[cursor_row][i] = lines[cursor_row][i + 1];
        cursor_col--;
        modified = true;
    } else if (cursor_row > 0) {
        /* Join with previous line */
        int prev_len = strlen(lines[cursor_row - 1]);
        int cur_len = strlen(lines[cursor_row]);
        if (prev_len + cur_len < EDITOR_LINE_LEN - 1) {
            strcat(lines[cursor_row - 1], lines[cursor_row]);
            /* Shift all lines up */
            for (int i = cursor_row; i < num_lines - 1; i++)
                strcpy(lines[i], lines[i + 1]);
            num_lines--;
            lines[num_lines][0] = '\0';
            cursor_row--;
            cursor_col = prev_len;
            modified = true;
        }
    }
}

static void editor_enter(void) {
    if (num_lines >= EDITOR_MAX_LINES) return;

    /* Shift lines down */
    for (int i = num_lines; i > cursor_row + 1; i--)
        strcpy(lines[i], lines[i - 1]);
    num_lines++;

    /* Split current line */
    int len = strlen(lines[cursor_row]);
    strcpy(lines[cursor_row + 1], lines[cursor_row] + cursor_col);
    lines[cursor_row][cursor_col] = '\0';
    (void)len;

    cursor_row++;
    cursor_col = 0;
    modified = true;
}

static void editor_cut_line(void) {
    static char cut_buffer[EDITOR_LINE_LEN];
    strcpy(cut_buffer, lines[cursor_row]);
    if (num_lines > 1) {
        for (int i = cursor_row; i < num_lines - 1; i++)
            strcpy(lines[i], lines[i + 1]);
        num_lines--;
        lines[num_lines][0] = '\0';
        if (cursor_row >= num_lines && cursor_row > 0)
            cursor_row--;
    } else {
        lines[0][0] = '\0';
    }
    cursor_col = 0;
    modified = true;
    /* Store in global for paste - we'll use a static for simplicity */
    (void)cut_buffer;
}

static void editor_save(void) {
    /* Build file content */
    char buf[RAMFS_MAX_DATA];
    int pos = 0;
    for (int i = 0; i < num_lines && pos < RAMFS_MAX_DATA - 2; i++) {
        int len = strlen(lines[i]);
        if (pos + len + 1 >= RAMFS_MAX_DATA) break;
        memcpy(buf + pos, lines[i], len);
        pos += len;
        buf[pos++] = '\n';
    }
    ramfs_write(edit_filename, buf, pos);
    modified = false;
}

void editor_open(const char* filename) {
    memset(lines, 0, sizeof(lines));
    num_lines = 1;
    cursor_row = cursor_col = scroll_offset = 0;
    modified = false;
    strncpy(edit_filename, filename, sizeof(edit_filename) - 1);

    /* Try to load file */
    char buf[RAMFS_MAX_DATA];
    int32_t n = ramfs_read(filename, buf, sizeof(buf));
    if (n > 0) {
        num_lines = 0;
        int col = 0;
        for (int32_t i = 0; i < n && num_lines < EDITOR_MAX_LINES; i++) {
            if (buf[i] == '\n') {
                lines[num_lines][col] = '\0';
                num_lines++;
                col = 0;
            } else if (col < EDITOR_LINE_LEN - 1) {
                lines[num_lines][col++] = buf[i];
            }
        }
        if (col > 0 || num_lines == 0) {
            lines[num_lines][col] = '\0';
            num_lines++;
        }
    }

    /* Editor main loop */
    terminal_clear();
    editor_draw_status();
    editor_draw_text();
    editor_update_cursor();

    while (1) {
        unsigned char c = (unsigned char)keyboard_getchar();

        if (c == 17) { /* Ctrl+Q */
            terminal_clear();
            return;
        }
        if (c == 19) { /* Ctrl+S */
            editor_save();
        } else if (c == 11) { /* Ctrl+K */
            editor_cut_line();
        } else if (c == '\n') {
            editor_enter();
        } else if (c == '\b') {
            editor_backspace();
        } else if (c == KEY_UP && cursor_row > 0) {
            cursor_row--;
            int len = strlen(lines[cursor_row]);
            if (cursor_col > len) cursor_col = len;
        } else if (c == KEY_DOWN && cursor_row < num_lines - 1) {
            cursor_row++;
            int len = strlen(lines[cursor_row]);
            if (cursor_col > len) cursor_col = len;
        } else if (c == KEY_LEFT) {
            if (cursor_col > 0) cursor_col--;
            else if (cursor_row > 0) { cursor_row--; cursor_col = strlen(lines[cursor_row]); }
        } else if (c == KEY_RIGHT) {
            int len = strlen(lines[cursor_row]);
            if (cursor_col < len) cursor_col++;
            else if (cursor_row < num_lines - 1) { cursor_row++; cursor_col = 0; }
        } else if (c == KEY_HOME) {
            cursor_col = 0;
        } else if (c == KEY_END) {
            cursor_col = strlen(lines[cursor_row]);
        } else if (c == KEY_PGUP) {
            cursor_row -= EDIT_ROWS;
            if (cursor_row < 0) cursor_row = 0;
        } else if (c == KEY_PGDOWN) {
            cursor_row += EDIT_ROWS;
            if (cursor_row >= num_lines) cursor_row = num_lines - 1;
        } else if (c >= 32 && c != 127) {
            editor_insert_char(c);
        } else if (c == '\t') {
            for (int i = 0; i < 4; i++) editor_insert_char(' ');
        }

        editor_draw_status();
        editor_draw_text();
        editor_update_cursor();
    }
}
