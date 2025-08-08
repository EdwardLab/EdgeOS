#include "fb.h"
#include "string.h"
#include <stddef.h>
#include "console.h"

fb_t fb = {0};

static uint8_t *fb_backbuffer = NULL;
#define FB_BACKBUFFER_MAX (1024*768*4)
static uint8_t fb_backbuffer_storage[FB_BACKBUFFER_MAX]; /* up to 1024x768x32 */

#define ALIGN_UP(v, a) (((v) + (a) - 1) & ~((a) - 1))

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
            return true;
        }
        tag = (struct multiboot_tag *)((uint8_t *)tag + ALIGN_UP(tag->size, 8));
    }
    return false;
}

void fb_debug_dump(void) {
    console_printf("FB addr=%p size=%ux%u pitch=%u bpp=%u\n",
                   fb.addr, fb.width, fb.height, fb.pitch, fb.bpp);
    console_printf("R: mask=%08x pos=%u\n", fb.r_mask, fb.r_pos);
    console_printf("G: mask=%08x pos=%u\n", fb.g_mask, fb.g_pos);
    console_printf("B: mask=%08x pos=%u\n", fb.b_mask, fb.b_pos);
}

void fb_putpixel(int x, int y, uint32_t argb) {
    if (!fb.addr)
        return;
    if ((unsigned)x >= fb.width || (unsigned)y >= fb.height)
        return;
    uint8_t *base = fb_backbuffer ? fb_backbuffer : fb.addr;
    uint32_t pixel = fb_argb_to_pixel(argb);
    *(uint32_t *)(base + y * fb.pitch + x * (fb.bpp / 8)) = pixel;
}

void fb_clear(uint32_t argb) {
    if (!fb.addr)
        return;
    uint8_t *base = fb_backbuffer ? fb_backbuffer : fb.addr;
    uint32_t pixel = fb_argb_to_pixel(argb);
    for (uint32_t y = 0; y < fb.height; y++) {
        uint32_t *row = (uint32_t *)(base + y * fb.pitch);
        for (uint32_t x = 0; x < fb.width; x++) {
            row[x] = pixel;
        }
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
