#ifndef FB_H
#define FB_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t *addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t r_mask, g_mask, b_mask;
    uint32_t r_pos, g_pos, b_pos;
} fb_t;

extern fb_t fb;

bool fb_init_from_multiboot2(void *mb_info);
void fb_debug_dump(void);
void fb_putpixel(int x, int y, uint32_t argb);
void fb_clear(uint32_t argb);
bool fb_enable_backbuffer(void);
void fb_present(void);

#endif /* FB_H */
