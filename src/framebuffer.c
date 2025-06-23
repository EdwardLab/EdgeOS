#include "multiboot.h"
#include "stdint-gcc.h"
#include "ctypes.h"
#include "qemu.h"
#include "font8x8_basic.h"
#include "framebuffer.h"


multiboot_uint32_t* framebuffer_buffer;
multiboot_uint32_t framebuffer_bpp;
multiboot_uint32_t framebuffer_pitch;
multiboot_uint32_t framebuffer_height;
multiboot_uint32_t framebuffer_width;

uint32_t cur_x = 0;
uint32_t cur_y = 0;

uint32_t fg_color, bg_color;
//Define Macros
#define CHECK_FLAG(flags,bit)   ((flags) & (1 << (bit)))

int framebuffer_check(multiboot_info_t* multiboot){
  int framebuffer_type;
  if (CHECK_FLAG (multiboot->flags, 12)){
    
    switch (multiboot->framebuffer_type){
      case MULTIBOOT_FRAMEBUFFER_TYPE_RGB:
        
        framebuffer_type = 1;
        return framebuffer_type;
    }
    return framebuffer_type;
  }
  return framebuffer_type;
}

int init_framebuffer(multiboot_info_t* mbi){
  if(framebuffer_check(mbi)==1){
    framebuffer_buffer = (uint32_t*)((uintptr_t)mbi->framebuffer_addr);
    framebuffer_bpp = mbi->framebuffer_bpp;
    framebuffer_pitch = mbi->framebuffer_pitch;
    framebuffer_height = mbi->framebuffer_height;
    framebuffer_width = mbi->framebuffer_width;
  }
  return 0;
}

void framebuffer_putpixel(uint32_t x, uint32_t y, uint32_t color){
    if (x >= framebuffer_width || y >= framebuffer_height)
        return;
    framebuffer_buffer[y * framebuffer_width + x] = color;
}

void framebuffer_putchar(char ch, uint32_t color){
    if (ch == '\n') {
        cur_x = 0;
        cur_y++;
        return;
    }
    for (int row = 0; row < FONT_HEIGHT; row++) {
        unsigned char bits = font8x8_basic[(unsigned char)ch][row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            uint32_t pixel_color = (bits & (1 << col)) ? color : bg_color;
            framebuffer_putpixel(cur_x * FONT_WIDTH + col,
                                 cur_y * FONT_HEIGHT + row,
                                 pixel_color);
        }
    }
    cur_x++;
    if (cur_x >= framebuffer_width / FONT_WIDTH) {
        cur_x = 0;
        cur_y++;
    }
    if (cur_y >= framebuffer_height / FONT_HEIGHT) {
        cur_y = framebuffer_height / FONT_HEIGHT - 1;
    }
}

void framebuffer_backspace(uint32_t color){
    (void)color;
    if (cur_x == 0) return;
    cur_x--;
    for (int row = 0; row < FONT_HEIGHT; row++) {
        for (int col = 0; col < FONT_WIDTH; col++) {
            framebuffer_putpixel(cur_x * FONT_WIDTH + col,
                                 cur_y * FONT_HEIGHT + row,
                                 bg_color);
        }
    }
}

void framebuffer_back(){
    cur_x--;
}

void framebuffer_putstr(char *str, uint32_t color){
    while(*str!=0){
        framebuffer_putchar(*str, color);
        str++;
    }
}

void framebuffer_clscr(uint32_t color){
    for(uint32_t i = 0; i < (framebuffer_height * framebuffer_width); i++){
        framebuffer_buffer[i] = color;
    }
    cur_x = 0;
    cur_y = 0;
}

void init_tty(multiboot_info_t *mbi, uint32_t fg, uint32_t bg){
  init_framebuffer(mbi);
  fg_color = fg;
  bg_color = bg;
  framebuffer_clscr(bg);
}

void print_char(char ch){
	framebuffer_putchar(ch, fg_color);
}

void gotoxy(uint32_t x, uint32_t y){
	cur_x=x;
	cur_y=y;
}
