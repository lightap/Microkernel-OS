#ifndef TYPES_H
#define TYPES_H

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;
typedef unsigned int       size_t;
typedef signed int         ssize_t;

#define NULL ((void*)0)
#define true  1
#define false 0
typedef int bool;

/* Port I/O */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

static inline void cli(void) { __asm__ volatile ("cli"); }
static inline void sti(void) { __asm__ volatile ("sti"); }
static inline void hlt(void) { __asm__ volatile ("hlt"); }

/* String utilities */
static inline size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static inline int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char*)a - *(unsigned char*)b;
}

static inline int strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

static inline char* strcpy(char* dst, const char* src) {
    char* d = dst;
    while ((*d++ = *src++));
    return dst;
}

static inline char* strncpy(char* dst, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

static inline char* strcat(char* dst, const char* src) {
    char* d = dst + strlen(dst);
    while ((*d++ = *src++));
    return dst;
}

static inline char* strchr(const char* s, int c) {
    while (*s) { if (*s == (char)c) return (char*)s; s++; }
    return (c == 0) ? (char*)s : NULL;
}

static inline void* memset(void* dst, int c, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

static inline void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dst;
}

static inline int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* p = a, *q = b;
    for (size_t i = 0; i < n; i++)
        if (p[i] != q[i]) return p[i] - q[i];
    return 0;
}

static inline void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

static inline int isdigit(char c) { return c >= '0' && c <= '9'; }
static inline int isalpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static inline int isspace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static inline char tolower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static inline char toupper(char c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static inline int atoi(const char* s) {
    int n = 0, neg = 0;
    while (isspace(*s)) s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (isdigit(*s)) n = n * 10 + (*s++ - '0');
    return neg ? -n : n;
}

#endif
