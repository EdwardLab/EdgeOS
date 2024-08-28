#include "console.h"
#include "string.h"
#include "types.h"
#include "vga.h"
#include "keyboard.h"

static uint16 *g_vga_buffer;
// Index for video buffer array
static uint32 g_vga_index;
// Cursor positions
static uint8 cursor_pos_x = 0, cursor_pos_y = 0;
// Fore & back color values
uint8 g_fore_color = COLOR_WHITE, g_back_color = COLOR_BLACK;
static uint16 g_temp_pages[MAXIMUM_PAGES][VGA_TOTAL_ITEMS];
uint32 g_current_temp_page = 0;

// Clear video buffer array
void console_clear(VGA_COLOR_TYPE fore_color, VGA_COLOR_TYPE back_color) {
    uint32 i;

    for (i = 0; i < VGA_TOTAL_ITEMS; i++) {
        g_vga_buffer[i] = vga_item_entry(NULL, fore_color, back_color);
    }
    g_vga_index = 0;
    cursor_pos_x = 0;
    cursor_pos_y = 0;
    vga_set_cursor_pos(cursor_pos_x, cursor_pos_y);
}

// Initialize console
void console_init(VGA_COLOR_TYPE fore_color, VGA_COLOR_TYPE back_color) {
    g_vga_buffer = (uint16 *)VGA_ADDRESS;
    g_fore_color = fore_color;
    g_back_color = back_color;
    cursor_pos_x = 0;
    cursor_pos_y = 0;
    console_clear(fore_color, back_color);
}

void console_scroll(int type) {
    uint32 i;
    if (type == SCROLL_UP) {
        // Scroll up
        if (g_current_temp_page > 0)
            g_current_temp_page--;
        g_current_temp_page %= MAXIMUM_PAGES;
        for (i = 0; i < VGA_TOTAL_ITEMS; i++) {
            g_vga_buffer[i] = g_temp_pages[g_current_temp_page][i];
        }
    } else {
        // Scroll down
        g_current_temp_page++;
        g_current_temp_page %= MAXIMUM_PAGES;
        for (i = 0; i < VGA_TOTAL_ITEMS; i++) {
            g_vga_buffer[i] = g_temp_pages[g_current_temp_page][i];
        }
    }
}

static void console_newline() {
    if (cursor_pos_y >= VGA_HEIGHT - 1) {
        // scroll screen
        for (int i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++) {
            g_vga_buffer[i] = g_vga_buffer[i + VGA_WIDTH];
        }
        for (int i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++) {
            g_vga_buffer[i] = vga_item_entry(' ', g_fore_color, g_back_color);
        }
        cursor_pos_y = VGA_HEIGHT - 1;
    } else {
        cursor_pos_y++;
    }
    cursor_pos_x = 0;
    g_vga_index = cursor_pos_y * VGA_WIDTH + cursor_pos_x;
    vga_set_cursor_pos(cursor_pos_x, cursor_pos_y);
}

// Assign ASCII character to video buffer
void console_putchar(char ch) {
    if (ch == '\t') {
        for(int i = 0; i < 4; i++) {
            g_vga_buffer[g_vga_index++] = vga_item_entry(' ', g_fore_color, g_back_color);
            cursor_pos_x++;
            if (cursor_pos_x >= VGA_WIDTH) {
                cursor_pos_x = 0;
                cursor_pos_y++;
            }
        }
    } else if (ch == '\n') {
        console_newline();
    } else {
        if (ch > 0) {
            g_vga_buffer[g_vga_index++] = vga_item_entry(ch, g_fore_color, g_back_color);
            cursor_pos_x++;
            if (cursor_pos_x >= VGA_WIDTH) {
                cursor_pos_x = 0;
                cursor_pos_y++;
            }
            if (cursor_pos_y >= VGA_HEIGHT) {
                console_newline();
            }
        }
    }
    vga_set_cursor_pos(cursor_pos_x, cursor_pos_y);
}

// Revert back the printed character and add 0 to it
void console_ungetchar() {
    if(g_vga_index > 0) {
        g_vga_index--;  // Move back the index first
        if(cursor_pos_x > 0) {
            cursor_pos_x--;
        } else if (cursor_pos_y > 0) {
            cursor_pos_y--;
            cursor_pos_x = VGA_WIDTH - 1;
        }
        g_vga_buffer[g_vga_index] = vga_item_entry(0, g_fore_color, g_back_color);
        vga_set_cursor_pos(cursor_pos_x, cursor_pos_y);
    }
}

// Revert back the printed character until n characters
void console_ungetchar_bound(uint8 n) {
    while (n-- > 0 && g_vga_index > 0) {
        console_ungetchar();
    }
}

void console_gotoxy(uint16 x, uint16 y) {
    g_vga_index = (80 * y) + x;
    cursor_pos_x = x;
    cursor_pos_y = y;
    vga_set_cursor_pos(cursor_pos_x, cursor_pos_y);
}

// Print string by calling print_char
void console_putstr(const char *str) {
    uint32 index = 0;
    while (str[index]) {
        if (str[index] == '\n')
            console_newline();
        else
            console_putchar(str[index]);
        index++;
    }
}

void printf(const char *format, ...) {
    g_fore_color = COLOR_WHITE;
    char **arg = (char **)&format;
    int c;
    char buf[32];

    arg++;

    memset(buf, 0, sizeof(buf));
    while ((c = *format++) != 0) {
        if (c != '%')
            console_putchar(c);
        else {
            char *p, *p2;
            int pad0 = 0, pad = 0;

            c = *format++;
            if (c == '0') {
                pad0 = 1;
                c = *format++;
            }

            if (c >= '0' && c <= '9') {
                pad = c - '0';
                c = *format++;
            }

            switch (c) {
                case 'd':
                case 'u':
                case 'x':
                    itoa(buf, c, *((int *)arg++));
                    p = buf;
                    goto string;
                    break;

                case 's':
                    p = *arg++;
                    if (!p)
                        p = "(null)";

                string:
                    for (p2 = p; *p2; p2++)
                        ;
                    for (; p2 < p + pad; p2++)
                        console_putchar(pad0 ? '0' : ' ');
                    while (*p)
                        console_putchar(*p++);
                    break;

                default:
                    console_putchar(*((int *)arg++));
                    break;
            }
        }
    }
}

void printf_color(char vga_color, const char *format, ...) {
    g_fore_color = vga_color;
    char **arg = (char **)&format;
    int c;
    char buf[32];

    arg++;

    memset(buf, 0, sizeof(buf));
    while ((c = *format++) != 0) {
        if (c != '%') {
            console_putchar(c);
        } else {
            char *p, *p2;
            int pad0 = 0, pad = 0;

            c = *format++;
            if (c == '0') {
                pad0 = 1;
                c = *format++;
            }

            if (c >= '0' && c <= '9') {
                pad = c - '0';
                c = *format++;
            }

            switch (c) {
                case 'd':
                case 'u':
                case 'x':
                    itoa(buf, c, *((int *)arg++));
                    p = buf;
                    goto string;
                    break;

                case 's':
                    p = *arg++;
                    if (!p)
                        p = "(null)";

                string:
                    for (p2 = p; *p2; p2++)
                        ;
                    for (; p2 < p + pad; p2++)
                        console_putchar(pad0 ? '0' : ' ');
                    while (*p)
                        console_putchar(*p++);
                    break;

                default:
                    console_putchar(*((int *)arg++));
                    break;
            }
        }
    }
}

// Read string from console, but no backing
void getstr(char *buffer) {
    if (!buffer) return;
    while(1) {
        char ch = kb_getchar();
        if (ch == '\n') {
            printf("\n");
            return ;
        } else {
            *buffer++ = ch;
            printf("%c", ch);
        }
    }
}

// Read string from console, and erase or go back until bound occurs
void getstr_bound(char *buffer, uint8 bound) {
    if (!buffer) return;
    while(1) {
        char ch = kb_getchar();
        if (ch == '\n') {
            printf("\n");
            return ;
        } else if(ch == '\b') {
            if (buffer > 0) {
                console_ungetchar_bound(1);
                buffer--;
                *buffer = '\0';
            }
        } else {
            if (buffer - buffer < bound) {
                *buffer++ = ch;
                printf("%c", ch);
            }
        }
    }
}

uint8 get_cursor_x() {
    return cursor_pos_x;
}


uint8 get_cursor_y() {
    return cursor_pos_y;
}