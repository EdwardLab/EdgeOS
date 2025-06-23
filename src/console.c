#include "console.h"
#include "framebuffer.h"
#include "keyboard.h"
#include "multiboot.h"
#include <string.h>
#include <stdint.h>

extern multiboot_info_t* g_mbi;
extern uint32_t fg_color, bg_color;
extern uint32_t cur_x, cur_y;
void framebuffer_backspace(uint32_t color);

static const uint32_t color_table[] = {
    0x000000,0x0000AA,0x00AA00,0x00AAAA,
    0xAA0000,0xAA00AA,0xAA5500,0xAAAAAA,
    0x555555,0x5555FF,0x55FF55,0x55FFFF,
    0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF
};

void console_clear(VGA_COLOR_TYPE fore, VGA_COLOR_TYPE back){
    fg_color = color_table[fore];
    bg_color = color_table[back];
    framebuffer_clscr(bg_color);
    gotoxy(0,0);
}

void console_init(VGA_COLOR_TYPE fore, VGA_COLOR_TYPE back){
    init_tty(g_mbi, color_table[fore], color_table[back]);
}

void console_scroll(int line){
    (void)line; /* no-op */
}

void console_putchar(char ch){
    if(ch=='\b'){
        framebuffer_backspace(fg_color);
    } else {
        framebuffer_putchar(ch, fg_color);
    }
}

void console_ungetchar(){
    framebuffer_backspace(fg_color);
}

void console_ungetchar_bound(uint8 n){
    while(n--) framebuffer_backspace(fg_color);
}

void console_gotoxy(uint16 x, uint16 y){
    gotoxy(x,y);
}

void console_putstr(const char *str){
    while(*str){
        console_putchar(*str++);
    }
}

void printf(const char *format, ...){
    fg_color = COLOR_WHITE;
    char **arg = (char **)&format;
    int c;
    char buf[32];
    arg++;
    memset(buf,0,sizeof(buf));
    while((c=*format++)!=0){
        if(c!='%')
            console_putchar(c);
        else {
            char *p,*p2; int pad0=0,pad=0;
            c=*format++;
            if(c=='0'){ pad0=1; c=*format++; }
            if(c>='0'&&c<='9'){ pad=c-'0'; c=*format++; }
            switch(c){
                case 'd':
                case 'u':
                case 'x':
                    itoa(buf,c,*((int *)arg++));
                    p=buf; goto string;
                case 's':
                    p=*arg++; if(!p) p="(null)";
            string:
                    for(p2=p;*p2;p2++); for(;p2<p+pad;p2++) console_putchar(pad0?'0':' ');
                    while(*p) console_putchar(*p++);
                    break;
                default:
                    console_putchar(*((int *)arg++));
                    break;
            }
        }
    }
}

void printf_color(char vga_color, const char *format, ...){
    fg_color = vga_color;
    char **arg = (char **)&format;
    int c; char buf[32];
    arg++; memset(buf,0,sizeof(buf));
    while((c=*format++)!=0){
        if(c!='%') console_putchar(c); else {
            char *p,*p2; int pad0=0,pad=0;
            c=*format++; if(c=='0'){ pad0=1; c=*format++; }
            if(c>='0'&&c<='9'){ pad=c-'0'; c=*format++; }
            switch(c){
                case 'd':
                case 'u':
                case 'x':
                    itoa(buf,c,*((int *)arg++)); p=buf; goto string2;
                case 's':
                    p=*arg++; if(!p) p="(null)";
            string2:
                    for(p2=p;*p2;p2++); for(;p2<p+pad;p2++) console_putchar(pad0?'0':' ');
                    while(*p) console_putchar(*p++); break;
                default:
                    console_putchar(*((int *)arg++)); break;
            }
        }
    }
}

void getstr(char *buffer){
    if(!buffer) return;
    char *ptr = buffer;
    while(1){
        char ch = kb_getchar();
        if(ch=='\n'){
            printf("\n");
            *ptr = '\0';
            return;
        } else {
            *ptr++ = ch;
            printf("%c", ch);
        }
    }
}

void getstr_bound(char *buffer, uint8 bound){
    if(!buffer) return;
    uint8 i = 0;
    while(1){
        char ch = kb_getchar();
        if(ch=='\n'){
            printf("\n");
            buffer[i]='\0';
            return;
        } else if(ch=='\b'){
            if(i>0){
                console_ungetchar_bound(1);
                i--; 
            }
        } else {
            if(i < bound-1){
                buffer[i++] = ch;
                printf("%c", ch);
            }
        }
    }
}

uint8 get_cursor_x(){ return cur_x; }
uint8 get_cursor_y(){ return cur_y; }



