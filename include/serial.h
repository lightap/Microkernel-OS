#ifndef SERIAL_H
#define SERIAL_H

#include "types.h"

#define COM1 0x3F8
#define COM2 0x2F8

void serial_init(uint16_t port);
void serial_putchar(uint16_t port, char c);
void serial_print(uint16_t port, const char* str);
void serial_printf(const char* fmt, ...);  /* To COM1 */
char serial_read(uint16_t port);
bool serial_received(uint16_t port);

#endif
