/* mem.c - Non-inline memory functions for GCC-generated calls.
 * GCC sometimes emits calls to memset/memcpy for struct assignments,
 * large initializations, etc. The static inline versions in types.h
 * don't produce linkable symbols, so we provide them here. */

typedef unsigned int size_t;
typedef unsigned char uint8_t;

void* memset(void* dst, int c, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* p = a, *q = b;
    for (size_t i = 0; i < n; i++)
        if (p[i] != q[i]) return p[i] - q[i];
    return 0;
}
