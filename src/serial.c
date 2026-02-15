#include "serial.h"
#include "vga.h"

void serial_init(uint16_t port) {
    outb(port + 1, 0x00);    /* Disable interrupts */
    outb(port + 3, 0x80);    /* Enable DLAB */
    outb(port + 0, 0x03);    /* Baud 38400 (lo) */
    outb(port + 1, 0x00);    /* Baud 38400 (hi) */
    outb(port + 3, 0x03);    /* 8 bits, no parity, 1 stop */
    outb(port + 2, 0xC7);    /* Enable FIFO, 14-byte threshold */
    outb(port + 4, 0x0B);    /* IRQs enabled, RTS/DSR set */
}

static int is_transmit_empty(uint16_t port) {
    return inb(port + 5) & 0x20;
}

void serial_putchar(uint16_t port, char c) {
    while (!is_transmit_empty(port));
    outb(port, c);
}

void serial_print(uint16_t port, const char* str) {
    while (*str) {
        if (*str == '\n') serial_putchar(port, '\r');
        serial_putchar(port, *str++);
    }
}

bool serial_received(uint16_t port) {
    return inb(port + 5) & 1;
}

char serial_read(uint16_t port) {
    while (!serial_received(port));
    return inb(port);
}

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)

static void serial_print_dec(uint16_t port, uint32_t val) {
    if (val == 0) { serial_putchar(port, '0'); return; }
    char buf[12]; int i = 0;
    while (val) { buf[i++] = '0' + val % 10; val /= 10; }
    while (--i >= 0) serial_putchar(port, buf[i]);
}

static void serial_print_hex(uint16_t port, uint32_t val) {
    const char h[] = "0123456789ABCDEF";
    serial_print(port, "0x");
    for (int i = 28; i >= 0; i -= 4) serial_putchar(port, h[(val >> i) & 0xF]);
}

void serial_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 's': serial_print(COM1, va_arg(args, const char*)); break;
                case 'd': serial_print_dec(COM1, va_arg(args, uint32_t)); break;
                case 'u': serial_print_dec(COM1, va_arg(args, uint32_t)); break;
                case 'x': serial_print_hex(COM1, va_arg(args, uint32_t)); break;
                case 'p': serial_print_hex(COM1, va_arg(args, uint32_t)); break;
                case 'c': serial_putchar(COM1, (char)va_arg(args, int)); break;
                case '%': serial_putchar(COM1, '%'); break;
                default: serial_putchar(COM1, *fmt); break;
            }
        } else {
            if (*fmt == '\n') serial_putchar(COM1, '\r');
            serial_putchar(COM1, *fmt);
        }
        fmt++;
    }
    va_end(args);
}
