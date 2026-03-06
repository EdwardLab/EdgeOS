#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define FBIOGET_VSCREENINFO 0x4600u
#define FBIOGET_FSCREENINFO 0x4602u

struct fb_bitfield_u {
    uint32_t offset, length, msb_right;
};

struct fb_fix_screeninfo_u {
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

struct fb_var_screeninfo_u {
    uint32_t xres, yres, xres_virtual, yres_virtual;
    uint32_t xoffset, yoffset, bits_per_pixel, grayscale;
    struct fb_bitfield_u red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len, sync, vmode, rotate, colorspace;
    uint32_t reserved[4];
};

static uint32_t pack_rgb(uint32_t r, uint32_t g, uint32_t b,
                         const struct fb_var_screeninfo_u *v) {
    uint32_t p = 0;
    if (v->red.length)   p |= ((r >> (8 - v->red.length)) << v->red.offset);
    if (v->green.length) p |= ((g >> (8 - v->green.length)) << v->green.offset);
    if (v->blue.length)  p |= ((b >> (8 - v->blue.length)) << v->blue.offset);
    return p;
}

static void putpx(uint8_t *buf, uint32_t pitch, uint32_t bpp, int x, int y, uint32_t pix) {
    uint8_t *p = buf + (uint32_t)y * pitch + (uint32_t)x * ((bpp + 7u) / 8u);
    if (bpp == 32) {
        *(uint32_t *)p = pix;
    } else if (bpp == 24) {
        p[0] = (uint8_t)(pix & 0xFFu);
        p[1] = (uint8_t)((pix >> 8) & 0xFFu);
        p[2] = (uint8_t)((pix >> 16) & 0xFFu);
    } else if (bpp == 16) {
        *(uint16_t *)p = (uint16_t)pix;
    } else {
        *p = (uint8_t)pix;
    }
}

static void fill_rect(uint8_t *buf, uint32_t pitch, uint32_t bpp,
                      int x, int y, int w, int h, uint32_t pix,
                      uint32_t maxw, uint32_t maxh) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > (int)maxw) x1 = (int)maxw;
    if (y1 > (int)maxh) y1 = (int)maxh;
    for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) putpx(buf, pitch, bpp, xx, yy, pix);
    }
}

int main(int argc, char **argv) {
    int fd = open("/dev/fb0", O_RDWR);
    struct fb_fix_screeninfo_u fix;
    struct fb_var_screeninfo_u var;
    uint8_t *buf;
    size_t sz;
    int sleep_ms = 5000;
    if (argc > 1) sleep_ms = atoi(argv[1]);
    if (sleep_ms < 0) sleep_ms = 0;
    if (fd < 0) {
        perror("open /dev/fb0");
        return 1;
    }
    memset(&fix, 0, sizeof(fix));
    memset(&var, 0, sizeof(var));
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
        perror("fb ioctl");
        close(fd);
        return 1;
    }
    sz = fix.smem_len ? fix.smem_len : (size_t)fix.line_length * var.yres;
    buf = (uint8_t *)malloc(sz);
    if (!buf) {
        perror("malloc");
        close(fd);
        return 1;
    }
    memset(buf, 0, sz);

    uint32_t bg = pack_rgb(24, 28, 36, &var);
    uint32_t desk = pack_rgb(34, 64, 94, &var);
    uint32_t shadow = pack_rgb(15, 18, 22, &var);
    uint32_t win = pack_rgb(228, 231, 236, &var);
    uint32_t title = pack_rgb(45, 115, 220, &var);
    uint32_t border = pack_rgb(90, 96, 108, &var);
    uint32_t btn = pack_rgb(220, 80, 80, &var);

    fill_rect(buf, fix.line_length, var.bits_per_pixel, 0, 0, (int)var.xres, (int)var.yres, bg, var.xres, var.yres);
    fill_rect(buf, fix.line_length, var.bits_per_pixel, 12, 12, (int)var.xres - 24, (int)var.yres - 24, desk, var.xres, var.yres);

    int ww = (int)var.xres * 3 / 5;
    int wh = (int)var.yres * 3 / 5;
    if (ww < 220) ww = (int)var.xres - 40;
    if (wh < 140) wh = (int)var.yres - 40;
    int wx = ((int)var.xres - ww) / 2;
    int wy = ((int)var.yres - wh) / 2;

    fill_rect(buf, fix.line_length, var.bits_per_pixel, wx + 8, wy + 8, ww, wh, shadow, var.xres, var.yres);
    fill_rect(buf, fix.line_length, var.bits_per_pixel, wx, wy, ww, wh, border, var.xres, var.yres);
    fill_rect(buf, fix.line_length, var.bits_per_pixel, wx + 2, wy + 2, ww - 4, wh - 4, win, var.xres, var.yres);
    fill_rect(buf, fix.line_length, var.bits_per_pixel, wx + 2, wy + 2, ww - 4, 26, title, var.xres, var.yres);
    fill_rect(buf, fix.line_length, var.bits_per_pixel, wx + ww - 26, wy + 7, 14, 14, btn, var.xres, var.yres);
    fill_rect(buf, fix.line_length, var.bits_per_pixel, wx + 20, wy + 50, ww - 40, 2, border, var.xres, var.yres);
    fill_rect(buf, fix.line_length, var.bits_per_pixel, wx + 20, wy + 72, ww - 40, 2, border, var.xres, var.yres);
    fill_rect(buf, fix.line_length, var.bits_per_pixel, wx + 20, wy + 94, ww - 40, 2, border, var.xres, var.yres);

    if (write(fd, buf, sz) < 0) {
        perror("write /dev/fb0");
        free(buf);
        close(fd);
        return 1;
    }

    printf("fbwin: drew test window on /dev/fb0 (%ux%u %ubpp), sleeping %d ms\n",
           var.xres, var.yres, var.bits_per_pixel, sleep_ms);
    if (sleep_ms > 0) usleep((useconds_t)sleep_ms * 1000u);
    free(buf);
    close(fd);
    return 0;
}
