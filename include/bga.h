#ifndef BGA_H
#define BGA_H

#include "types.h"

/* Bochs/QEMU VGA Extended Graphics Adapter driver */

bool     bga_available(void);
bool     bga_set_mode(uint16_t width, uint16_t height, uint8_t bpp);
void     bga_disable(void);
uint8_t* bga_get_fb(void);     /* Virtual address of mapped framebuffer */
uint16_t bga_get_width(void);
uint16_t bga_get_height(void);

#endif
