#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define FBIOGET_VSCREENINFO 0x4600u
#define FBIOGET_FSCREENINFO 0x4602u

struct fb_bitfield_u { uint32_t offset, length, msb_right; };
struct fb_fix_screeninfo_u {
    char id[16]; uint64_t smem_start; uint32_t smem_len; uint32_t type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep; uint32_t line_length; uint64_t mmio_start;
    uint32_t mmio_len, accel; uint16_t capabilities; uint16_t reserved[2];
};
struct fb_var_screeninfo_u {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset, bits_per_pixel, grayscale;
    struct fb_bitfield_u red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin, hsync_len, vsync_len;
    uint32_t sync, vmode, rotate, colorspace, reserved[4];
};

static uint32_t pack_rgb(uint32_t r, uint32_t g, uint32_t b, const struct fb_var_screeninfo_u *v) {
    uint32_t p = 0;
    if (v->red.length) p |= ((r >> (8 - v->red.length)) << v->red.offset);
    if (v->green.length) p |= ((g >> (8 - v->green.length)) << v->green.offset);
    if (v->blue.length) p |= ((b >> (8 - v->blue.length)) << v->blue.offset);
    return p;
}

static void putpx(uint8_t *buf, uint32_t pitch, uint32_t bpp, int x, int y, uint32_t pix) {
    uint8_t *p = buf + (uint32_t)y * pitch + (uint32_t)x * ((bpp + 7u) / 8u);
    if (bpp == 32) *(uint32_t *)p = pix;
    else if (bpp == 24) { p[0] = pix & 0xFFu; p[1] = (pix >> 8) & 0xFFu; p[2] = (pix >> 16) & 0xFFu; }
    else if (bpp == 16) *(uint16_t *)p = (uint16_t)pix;
    else *p = (uint8_t)pix;
}

static void fill_rect(uint8_t *buf, uint32_t pitch, uint32_t bpp, int x, int y, int w, int h, uint32_t pix, uint32_t maxw, uint32_t maxh) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w, y1 = y + h;
    if (x1 > (int)maxw) x1 = (int)maxw;
    if (y1 > (int)maxh) y1 = (int)maxh;
    for (int yy = y0; yy < y1; ++yy) for (int xx = x0; xx < x1; ++xx) putpx(buf, pitch, bpp, xx, yy, pix);
}

int main(int argc, char **argv) {
    int fd = open("/dev/fb0", O_RDWR);
    int sleep_ms = (argc > 1) ? atoi(argv[1]) : 5000;
    struct fb_fix_screeninfo_u fix;
    struct fb_var_screeninfo_u var;
    uint8_t *map;
    if (fd < 0) { perror("open /dev/fb0"); return 1; }
    memset(&fix, 0, sizeof(fix));
    memset(&var, 0, sizeof(var));
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
        perror("fb ioctl");
        close(fd);
        return 1;
    }
    map = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        perror("mmap /dev/fb0");
        close(fd);
        return 1;
    }
    uint32_t bg = pack_rgb(18, 22, 28, &var);
    uint32_t win = pack_rgb(235, 238, 242, &var);
    uint32_t title = pack_rgb(24, 112, 208, &var);
    uint32_t border = pack_rgb(70, 78, 90, &var);
    fill_rect(map, fix.line_length, var.bits_per_pixel, 0, 0, (int)var.xres, (int)var.yres, bg, var.xres, var.yres);
    int ww = (int)var.xres * 2 / 3, wh = (int)var.yres * 2 / 3;
    int wx = ((int)var.xres - ww) / 2, wy = ((int)var.yres - wh) / 2;
    fill_rect(map, fix.line_length, var.bits_per_pixel, wx, wy, ww, wh, border, var.xres, var.yres);
    fill_rect(map, fix.line_length, var.bits_per_pixel, wx + 2, wy + 2, ww - 4, wh - 4, win, var.xres, var.yres);
    fill_rect(map, fix.line_length, var.bits_per_pixel, wx + 2, wy + 2, ww - 4, 24, title, var.xres, var.yres);
    printf("fbmmapwin: mapped /dev/fb0 at %p len=%u, drew test window, sleeping %d ms\n", map, fix.smem_len, sleep_ms);
    if (sleep_ms > 0) usleep((useconds_t)sleep_ms * 1000u);
    munmap(map, fix.smem_len);
    close(fd);
    return 0;
}
