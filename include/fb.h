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
uint8_t *fb_get_draw_buffer(void);
void fb_fill_rect(int x,int y,int w,int h,uint32_t argb);
void fb_draw_mono_bitmap(int x,int y,int w,int h,const uint8_t*bits,int stride,
                         uint32_t fg,uint32_t bg,bool opaque);
void fb_draw_char(int x,int y,char ch,uint32_t fg,uint32_t bg,bool opaque);
void fb_draw_string(int x,int y,const char* s,uint32_t fg,uint32_t bg,bool opaque);
int fb_get_2m_remap(uint64_t *phys_base, uint32_t *page_count, uint64_t *virt_base);
int fb_get_2m_phys_window(uint64_t *phys_base, uint32_t *page_count, uint64_t *offset_in_first_page);

#endif /* FB_H */
