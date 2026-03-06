#include "fb.h"
#include "string.h"
#include <stddef.h>
#include "console.h"
#include "stdio.h" 
#include "font8x8_basic.h"
fb_t fb = {0};

static uint8_t *fb_backbuffer = NULL;
#define FB_BACKBUFFER_MAX (1024*768*4)
static uint8_t fb_backbuffer_storage[FB_BACKBUFFER_MAX]; /* up to 1024x768x32 */
#define FB_REMAP_VIRT_BASE 0x00000000FC000000ULL
#define FB_REMAP_MAX_PAGES 32
#define PAGE_PRESENT 0x001ULL
#define PAGE_WRITE   0x002ULL
#define PAGE_PS      0x080ULL

#define ALIGN_UP(v, a) (((v) + (a) - 1) & ~((a) - 1))
#ifndef FB_DEBUG
#define FB_DEBUG 0
#endif
static inline int fb_bpp_bytes(void) { return (fb.bpp + 7) / 8; }
static uint64_t g_fb_remap_phys_base;
static uint32_t g_fb_remap_pages;
static int g_fb_remap_active;
static uint64_t g_fb_phys_base;
static uint32_t g_fb_phys_pages;
static uint64_t g_fb_phys_offset;
extern uint64_t pd_table3[512];

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(uint64_t v) {
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(v) : "memory");
}

struct multiboot_tag {
    uint32_t type;
    uint32_t size;
};

struct multiboot_tag_framebuffer {
    struct multiboot_tag tag;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint16_t reserved;
    union {
        struct {
            uint32_t framebuffer_palette_addr;
            uint16_t framebuffer_palette_num_colors;
        };
        struct {
            uint8_t framebuffer_red_field_position;
            uint8_t framebuffer_red_mask_size;
            uint8_t framebuffer_green_field_position;
            uint8_t framebuffer_green_mask_size;
            uint8_t framebuffer_blue_field_position;
            uint8_t framebuffer_blue_mask_size;
        };
    };
};

static uint32_t fb_argb_to_pixel(uint32_t argb) {
    uint32_t r = (argb >> 16) & 0xFF;
    uint32_t g = (argb >> 8) & 0xFF;
    uint32_t b = argb & 0xFF;
    uint32_t pixel = (r << fb.r_pos) & fb.r_mask;
    pixel |= (g << fb.g_pos) & fb.g_mask;
    pixel |= (b << fb.b_pos) & fb.b_mask;
    return pixel;
}

bool fb_init_from_multiboot2(void *mb_info) {
    if (!mb_info)
        return false;
    uint8_t *ptr = (uint8_t *)mb_info;
    struct multiboot_tag *tag = (struct multiboot_tag *)(ptr + 8);
    while (tag->type != 0) {
        if (tag->type == 8) {
            struct multiboot_tag_framebuffer *fbtag = (struct multiboot_tag_framebuffer *)tag;
            if (fbtag->framebuffer_type != 1)
                return false;
            fb.addr = (uint8_t *)(uintptr_t)fbtag->framebuffer_addr;
            fb.pitch = fbtag->framebuffer_pitch;
            fb.width = fbtag->framebuffer_width;
            fb.height = fbtag->framebuffer_height;
            fb.bpp = fbtag->framebuffer_bpp;
            fb.r_pos = fbtag->framebuffer_red_field_position;
            fb.g_pos = fbtag->framebuffer_green_field_position;
            fb.b_pos = fbtag->framebuffer_blue_field_position;
            fb.r_mask = ((1u << fbtag->framebuffer_red_mask_size) - 1u) << fb.r_pos;
            fb.g_mask = ((1u << fbtag->framebuffer_green_mask_size) - 1u) << fb.g_pos;
            fb.b_mask = ((1u << fbtag->framebuffer_blue_mask_size) - 1u) << fb.b_pos;
            g_fb_remap_active = 0;
            g_fb_remap_phys_base = 0;
            g_fb_remap_pages = 0;
            {
                uint64_t phys = fbtag->framebuffer_addr;
                uint64_t phys_base = phys & ~0x1FFFFFULL;
                uint64_t offset = phys - phys_base;
                uint64_t size = (uint64_t)fb.pitch * (uint64_t)fb.height;
                uint64_t total = offset + size;
                uint32_t pages = (uint32_t)((total + 0x1FFFFFULL) >> 21);
                g_fb_phys_base = phys_base;
                g_fb_phys_offset = offset;
                g_fb_phys_pages = pages;
            }
            if (fbtag->framebuffer_addr > 0xFFFFFFFFULL) {
                uint64_t phys = fbtag->framebuffer_addr;
                uint64_t phys_base = phys & ~0x1FFFFFULL;
                uint64_t offset = phys - phys_base;
                uint64_t size = (uint64_t)fb.pitch * (uint64_t)fb.height;
                uint64_t total = offset + size;
                uint32_t pages = (uint32_t)((total + 0x1FFFFFULL) >> 21);
                if (pages == 0 || pages > FB_REMAP_MAX_PAGES) return false;
                for (uint32_t i = 0; i < pages; ++i) {
                    uint64_t p = phys_base + ((uint64_t)i << 21);
                    pd_table3[480 + i] = (p & 0x000FFFFFFFFFF000ULL) | PAGE_PRESENT | PAGE_WRITE | PAGE_PS;
                }
                for (uint32_t i = pages; i < FB_REMAP_MAX_PAGES; ++i) {
                    uint64_t p = 0xFC000000ULL + ((uint64_t)i << 21);
                    pd_table3[480 + i] = (p & 0x000FFFFFFFFFF000ULL) | PAGE_PRESENT | PAGE_WRITE | PAGE_PS;
                }
                write_cr3(read_cr3());
                fb.addr = (uint8_t *)(uintptr_t)(FB_REMAP_VIRT_BASE + offset);
                g_fb_remap_active = 1;
                g_fb_remap_phys_base = phys_base;
                g_fb_remap_pages = pages;
            }
            return true;
        }
        tag = (struct multiboot_tag *)((uint8_t *)tag + ALIGN_UP(tag->size, 8));
    }
    return false;
}

int fb_get_2m_remap(uint64_t *phys_base, uint32_t *page_count, uint64_t *virt_base) {
    if (!g_fb_remap_active) return 0;
    if (phys_base) *phys_base = g_fb_remap_phys_base;
    if (page_count) *page_count = g_fb_remap_pages;
    if (virt_base) *virt_base = FB_REMAP_VIRT_BASE;
    return 1;
}

int fb_get_2m_phys_window(uint64_t *phys_base, uint32_t *page_count, uint64_t *offset_in_first_page) {
    if (!fb.addr || fb.pitch == 0 || fb.height == 0 || g_fb_phys_pages == 0) return 0;
    if (phys_base) *phys_base = g_fb_phys_base;
    if (page_count) *page_count = g_fb_phys_pages;
    if (offset_in_first_page) *offset_in_first_page = g_fb_phys_offset;
    return 1;
}

void fb_debug_dump(void) {
#if FB_DEBUG
    printf("FB addr=%p size=%ux%u pitch=%u bpp=%u\n",
                   fb.addr, fb.width, fb.height, fb.pitch, fb.bpp);
    printf("R: mask=%08x pos=%u\n", fb.r_mask, fb.r_pos);
    printf("G: mask=%08x pos=%u\n", fb.g_mask, fb.g_pos);
    printf("B: mask=%08x pos=%u\n", fb.b_mask, fb.b_pos);
#endif
}



void fb_putpixel(int x, int y, uint32_t argb) {
    if (!fb.addr) return;
    if ((unsigned)x >= fb.width || (unsigned)y >= fb.height) return;
    uint8_t *base = fb_backbuffer ? fb_backbuffer : fb.addr;
    uint32_t pixel = fb_argb_to_pixel(argb);
    uint8_t *p = base + y * fb.pitch + x * fb_bpp_bytes();

    switch (fb.bpp) {
    case 32: *(uint32_t*)p = pixel; break;
    case 24: p[0] = (pixel) & 0xFF; p[1] = (pixel>>8) & 0xFF; p[2] = (pixel>>16) & 0xFF; break;
    case 16: *(uint16_t*)p = (uint16_t)pixel; break;
    default: p[0] = pixel & 0xFF; break;
    }
}

void fb_clear(uint32_t argb) {
    if (!fb.addr) return;
    uint8_t *base = fb_backbuffer ? fb_backbuffer : fb.addr;
    uint32_t pix = fb_argb_to_pixel(argb);

    if (fb.bpp == 32) {
        for (uint32_t y=0; y<fb.height; y++) {
            uint32_t *row = (uint32_t *)(base + y * fb.pitch);
            for (uint32_t x=0; x<fb.width; x++) row[x] = pix;
        }
    } else if (fb.bpp == 24) {
        uint8_t r = (pix>>16)&0xFF, g=(pix>>8)&0xFF, b=pix&0xFF;
        for (uint32_t y=0; y<fb.height; y++) {
            uint8_t *row = base + y * fb.pitch;
            for (uint32_t x=0; x<fb.width; x++) { row[0]=b; row[1]=g; row[2]=r; row += 3; }
        }
    } else if (fb.bpp == 16) {
        uint16_t v = (uint16_t)pix;
        for (uint32_t y=0; y<fb.height; y++) {
            uint16_t *row = (uint16_t *)(base + y * fb.pitch);
            for (uint32_t x=0; x<fb.width; x++) row[x] = v;
        }
    } else {
        for (uint32_t y=0; y<fb.height; y++)
            for (uint32_t x=0; x<fb.width; x++)
                fb_putpixel(x,y,argb);
    }
}

bool fb_enable_backbuffer(void) {
    size_t size = fb.pitch * fb.height;
    if (size > FB_BACKBUFFER_MAX)
        return false;
    fb_backbuffer = fb_backbuffer_storage;
    return true;
}

void fb_present(void) {
    if (!fb.addr || !fb_backbuffer)
        return;
    memcpy(fb.addr, fb_backbuffer, fb.pitch * fb.height);
}


uint8_t *fb_get_draw_buffer(void) {
    return fb_backbuffer ? fb_backbuffer : fb.addr;
}
void fb_draw_char(int x, int y, char ch, uint32_t fg, uint32_t bg, bool opaque) {
    const uint8_t *glyph = font8x8_basic[(unsigned char)ch];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (1 << col)) {
                fb_putpixel(x + col, y + row, fg);
            } else if (opaque) {
                fb_putpixel(x + col, y + row, bg);
            }
        }
    }
}

void fb_draw_string(int x, int y, const char *s, uint32_t fg, uint32_t bg, bool opaque) {
    if (!s) return;
    int cx = x;
    while (*s) {
        if (*s == '\n') {
            y += 8;
            cx = x;
            s++;
            continue;
        }
        fb_draw_char(cx, y, *s++, fg, bg, opaque);
        cx += 8;
    }
}
