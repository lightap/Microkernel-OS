#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "types.h"

/* Special key codes (> 0x80) */
#define KEY_UP      0x80
#define KEY_DOWN    0x81
#define KEY_LEFT    0x82
#define KEY_RIGHT   0x83
#define KEY_HOME    0x84
#define KEY_END     0x85
#define KEY_PGUP    0x86
#define KEY_PGDOWN  0x87
#define KEY_DELETE  0x88
#define KEY_INSERT  0x89
#define KEY_F1      0x90
#define KEY_F2      0x91
#define KEY_F3      0x92
#define KEY_F4      0x93
#define KEY_F5      0x94
#define KEY_F6      0x95
#define KEY_F7      0x96
#define KEY_F8      0x97
#define KEY_F9      0x98
#define KEY_F10     0x99
#define KEY_F11     0x9A
#define KEY_F12     0x9B

void    keyboard_init(void);
char    keyboard_getchar(void);       /* Blocking read */
char    keyboard_trychar(void);      /* Non-blocking read, returns 0 if no key */
bool    keyboard_haskey(void);         /* Non-blocking check */
void    keyboard_readline(char* buf, size_t max);  /* Read a line with editing */
bool    keyboard_get_shift(void);
bool    keyboard_get_ctrl(void);
bool    keyboard_get_alt(void);

#endif
