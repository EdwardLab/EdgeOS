#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
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

int main(void) {
    int fd = open("/dev/fb0", O_RDWR);
    struct fb_fix_screeninfo_u fix;
    struct fb_var_screeninfo_u var;
    if (fd < 0) {
        perror("open /dev/fb0");
        return 1;
    }
    memset(&fix, 0, sizeof(fix));
    memset(&var, 0, sizeof(var));
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        perror("ioctl(FBIOGET_FSCREENINFO)");
        close(fd);
        return 1;
    }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
        perror("ioctl(FBIOGET_VSCREENINFO)");
        close(fd);
        return 1;
    }
    printf("fb0 id=%s\n", fix.id);
    printf("res=%ux%u virt=%ux%u bpp=%u pitch=%u bytes=%u\n",
           var.xres, var.yres, var.xres_virtual, var.yres_virtual,
           var.bits_per_pixel, fix.line_length, fix.smem_len);
    printf("rgb offsets=%u/%u/%u lengths=%u/%u/%u\n",
           var.red.offset, var.green.offset, var.blue.offset,
           var.red.length, var.green.length, var.blue.length);
    close(fd);
    return 0;
}
