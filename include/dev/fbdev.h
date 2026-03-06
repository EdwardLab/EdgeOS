#ifndef DEV_FBDEV_H
#define DEV_FBDEV_H

#include <stdint.h>

/* Linux fbdev ioctl compatibility (subset used by simple framebuffer apps). */
#define LINUX_FBIOGET_VSCREENINFO 0x4600u
#define LINUX_FBIOGET_FSCREENINFO 0x4602u
#define EDGE_FBDEV_USER_BASE      0x0000000038000000ULL
#define EDGE_FBDEV_USER_MAX_PAGES 32u /* 32 * 2MiB = 64MiB user alias window */

/* Legacy EdgeOS fb ioctl kept for internal compatibility. */
#define FB_IOCTL_GET_INFO_LEGACY  0x46FFu

struct fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
};

struct edge_fb_bitfield {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
};

/* Layout matches Linux struct prefix used by userspace fb clients. */
struct edge_fb_fix_screeninfo {
    char id[16];
    uint64_t smem_start;
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

struct edge_fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    struct edge_fb_bitfield red;
    struct edge_fb_bitfield green;
    struct edge_fb_bitfield blue;
    struct edge_fb_bitfield transp;
    uint32_t nonstd;
    uint32_t activate;
    uint32_t height;
    uint32_t width;
    uint32_t accel_flags;
    uint32_t pixclock;
    uint32_t left_margin;
    uint32_t right_margin;
    uint32_t upper_margin;
    uint32_t lower_margin;
    uint32_t hsync_len;
    uint32_t vsync_len;
    uint32_t sync;
    uint32_t vmode;
    uint32_t rotate;
    uint32_t colorspace;
    uint32_t reserved[4];
};

#endif
