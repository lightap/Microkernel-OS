#ifndef IMAGE_H
#define IMAGE_H

#include "types.h"

#define IMG_MAX_W 2048
#define IMG_MAX_H 2048

typedef struct {
    int width, height;
    uint32_t* pixels;  /* Points to static buffer, RGB 0x00RRGGBB */
    bool valid;
} image_t;

/* Load image from raw file data. Extension hint used for format detection.
   Returns true on success. Pixels stored in internal static buffer. */
bool image_load(image_t* img, const uint8_t* data, uint32_t size, const char* name);

/* Load specific formats */
bool image_load_bmp(image_t* img, const uint8_t* data, uint32_t size);
bool image_load_tga(image_t* img, const uint8_t* data, uint32_t size);
bool image_load_png(image_t* img, const uint8_t* data, uint32_t size);

/* Create a test gradient image in ramfs for testing */
void image_create_test(void);

/* Check if filename has an image extension */
bool image_is_image(const char* name);

#endif
