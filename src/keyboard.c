#include "keyboard.h"
#include "idt.h"
#include "vga.h"

/* Key buffer */
#define KEY_BUF_SIZE 256
static volatile char key_buffer[KEY_BUF_SIZE];
static volatile uint32_t key_head = 0;
static volatile uint32_t key_tail = 0;

/* Modifier states */
static volatile bool shift_held = false;
static volatile bool ctrl_held  = false;
static volatile bool alt_held   = false;
static volatile bool caps_lock  = false;

/* UK QWERTY (ISO) scancode -> ASCII (set 1) */
static const char scancode_lower[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '#','z','x','c','v','b','n','m',',','.','/', 0,   /* 0x2B: # (UK) */
    '*', 0, ' ', 0,
    0,0,0,0,0,0,0,0,0,0, /* F1-F10 */
    0, 0, /* Num/Scroll lock */
    '7','8','9','-','4','5','6','+','1','2','3','0','.',
    0, 0, '\\', 0, 0  /* 0x56: \\ (ISO extra key) */
};

static const char scancode_upper[128] = {
    0,  27, '!','"','\x9c','$','%','^','&','*','(',')','_','+','\b', /* 0x03: " 0x04: £ */
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','@','~',          /* 0x28: @ */
    0,  '~','Z','X','C','V','B','N','M','<','>','?', 0,           /* 0x2B: ~ */
    '*', 0, ' ', 0,
    0,0,0,0,0,0,0,0,0,0,
    0, 0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',
    0, 0, '|', 0, 0  /* 0x56: | (shift+ISO key) */
};

static void buf_put(char c) {
    uint32_t next = (key_head + 1) % KEY_BUF_SIZE;
    if (next != key_tail) {
        key_buffer[key_head] = c;
        key_head = next;
    }
}

static void keyboard_callback(registers_t* regs) {
    (void)regs;
    uint8_t sc = inb(0x60);

    /* Handle extended scancodes (0xE0 prefix) */
    static bool extended = false;
    if (sc == 0xE0) { extended = true; return; }

    if (extended) {
        extended = false;
        switch (sc) {
            case 0x48: buf_put(KEY_UP); return;
            case 0x50: buf_put(KEY_DOWN); return;
            case 0x4B: buf_put(KEY_LEFT); return;
            case 0x4D: buf_put(KEY_RIGHT); return;
            case 0x47: buf_put(KEY_HOME); return;
            case 0x4F: buf_put(KEY_END); return;
            case 0x49: buf_put(KEY_PGUP); return;
            case 0x51: buf_put(KEY_PGDOWN); return;
            case 0x53: buf_put(KEY_DELETE); return;
            case 0x52: buf_put(KEY_INSERT); return;
        }
        return;
    }

    /* Key release */
    if (sc & 0x80) {
        uint8_t released = sc & 0x7F;
        if (released == 0x2A || released == 0x36) shift_held = false;
        if (released == 0x1D) ctrl_held = false;
        if (released == 0x38) alt_held = false;
        return;
    }

    /* Modifier keys */
    if (sc == 0x2A || sc == 0x36) { shift_held = true; return; }
    if (sc == 0x1D) { ctrl_held = true; return; }
    if (sc == 0x38) { alt_held = true; return; }
    if (sc == 0x3A) { caps_lock = !caps_lock; return; } /* Caps Lock toggle */

    /* Function keys */
    if (sc >= 0x3B && sc <= 0x44) { buf_put(KEY_F1 + (sc - 0x3B)); return; }
    if (sc == 0x57) { buf_put(KEY_F11); return; }
    if (sc == 0x58) { buf_put(KEY_F12); return; }

    /* Ctrl+key combos */
    if (ctrl_held && sc < 128) {
        char c = scancode_lower[sc];
        if (c >= 'a' && c <= 'z') { buf_put(c - 'a' + 1); return; } /* Ctrl+A=1, Ctrl+C=3, etc */
        if (c == 'l' - 'a' + 1 + 'a' - 1) { buf_put(12); return; } /* Ctrl+L */
    }

    if (sc >= 128) return;

    /* Normal keys */
    bool upper = shift_held;
    char c = scancode_lower[sc];
    if (c >= 'a' && c <= 'z') {
        upper = shift_held ^ caps_lock;
    }
    c = upper ? scancode_upper[sc] : scancode_lower[sc];
    if (c) buf_put(c);
}

void keyboard_init(void) {
    register_interrupt_handler(33, keyboard_callback);
}

bool keyboard_haskey(void) {
    return key_head != key_tail;
}

char keyboard_getchar(void) {
    while (!keyboard_haskey()) hlt();
    char c = key_buffer[key_tail];
    key_tail = (key_tail + 1) % KEY_BUF_SIZE;
    return c;
}

char keyboard_trychar(void) {
    if (!keyboard_haskey()) return 0;
    char c = key_buffer[key_tail];
    key_tail = (key_tail + 1) % KEY_BUF_SIZE;
    return c;
}

bool keyboard_get_shift(void) { return shift_held; }
bool keyboard_get_ctrl(void)  { return ctrl_held; }
bool keyboard_get_alt(void)   { return alt_held; }

void keyboard_readline(char* buf, size_t max) {
    size_t pos = 0;
    size_t len = 0;

    while (1) {
        unsigned char c = (unsigned char)keyboard_getchar();

        if (c == '\n') {
            terminal_putchar('\n');
            buf[len] = '\0';
            return;
        } else if (c == '\b') {
            if (pos > 0) {
                /* Shift chars left */
                for (size_t i = pos - 1; i < len - 1; i++)
                    buf[i] = buf[i + 1];
                pos--;
                len--;
                /* Redraw from cursor */
                terminal_backspace();
                int r, cl;
                terminal_get_cursor(&r, &cl);
                for (size_t i = pos; i < len; i++)
                    terminal_putchar(buf[i]);
                terminal_putchar(' ');
                terminal_set_cursor(r, cl);
            }
        } else if (c == KEY_LEFT) {
            if (pos > 0) {
                pos--;
                int r, cl;
                terminal_get_cursor(&r, &cl);
                terminal_set_cursor(r, cl - 1);
            }
        } else if (c == KEY_RIGHT) {
            if (pos < len) {
                pos++;
                int r, cl;
                terminal_get_cursor(&r, &cl);
                terminal_set_cursor(r, cl + 1);
            }
        } else if (c == 3) {
            /* Ctrl+C */
            terminal_print("^C\n");
            buf[0] = '\0';
            return;
        } else if (c == 12) {
            /* Ctrl+L - clear screen */
            terminal_clear();
            buf[0] = '\0';
            return;
        } else if (c >= 32 && c != 127 && len < max - 1) {
            /* Insert char at position */
            for (size_t i = len; i > pos; i--)
                buf[i] = buf[i - 1];
            buf[pos] = c;
            len++;
            pos++;
            /* Redraw from cursor */
            terminal_putchar(c);
            int r, cl;
            terminal_get_cursor(&r, &cl);
            for (size_t i = pos; i < len; i++)
                terminal_putchar(buf[i]);
            terminal_set_cursor(r, cl);
        }
    }
}
