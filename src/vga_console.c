#include "vga_console.h"

#include "string.h"
#include "vga.h"

#include <stdint.h>

#define VGA_COLS 80
#define VGA_ROWS 25

static volatile uint16_t *const g_vga = (volatile uint16_t *)(uintptr_t)VGA_ADDRESS;
static int g_col;
static int g_row;
static uint8_t g_fg = COLOR_WHITE;
static uint8_t g_bg = COLOR_BLACK;

static uint16_t mk_cell(char ch) {
    return vga_item_entry((uint8_t)ch, (VGA_COLOR_TYPE)g_fg, (VGA_COLOR_TYPE)g_bg);
}

static void move_hw_cursor(void) {
    vga_set_cursor_pos((uint8_t)g_col, (uint8_t)g_row);
}

static void clear_row(int row) {
    for (int c = 0; c < VGA_COLS; ++c) g_vga[row * VGA_COLS + c] = mk_cell(' ');
}

static void scroll_if_needed(void) {
    if (g_row < VGA_ROWS) return;
    memmove((void *)g_vga, (const void *)(g_vga + VGA_COLS), (VGA_ROWS - 1) * VGA_COLS * sizeof(uint16_t));
    clear_row(VGA_ROWS - 1);
    g_row = VGA_ROWS - 1;
}

static void be_init(uint32_t fg, uint32_t bg) {
    g_fg = (uint8_t)(fg & 0x0F);
    g_bg = (uint8_t)(bg & 0x0F);
    g_col = 0;
    g_row = 0;
    for (int r = 0; r < VGA_ROWS; ++r) clear_row(r);
    vga_set_cursor_underline();
    move_hw_cursor();
}

static void be_set_color(uint32_t fg, uint32_t bg) {
    g_fg = (uint8_t)(fg & 0x0F);
    g_bg = (uint8_t)(bg & 0x0F);
}

static void be_clear(void) {
    g_col = 0;
    g_row = 0;
    for (int r = 0; r < VGA_ROWS; ++r) clear_row(r);
    move_hw_cursor();
}

static void be_move_cursor(int x, int y) {
    if (x >= 0 && x < VGA_COLS) g_col = x;
    if (y >= 0 && y < VGA_ROWS) g_row = y;
    move_hw_cursor();
}

static void be_get_cursor(int *x, int *y) {
    if (x) *x = g_col;
    if (y) *y = g_row;
}

static int be_get_cols(void) { return VGA_COLS; }
static int be_get_rows(void) { return VGA_ROWS; }

static void be_erase_in_line(int mode) {
    int start = 0;
    int end = VGA_COLS - 1;
    if (mode == 0) start = g_col;
    else if (mode == 1) end = g_col;
    else if (mode == 2) { start = 0; end = VGA_COLS - 1; }
    else return;
    if (start < 0) start = 0;
    if (end >= VGA_COLS) end = VGA_COLS - 1;
    if (start > end) return;
    for (int c = start; c <= end; ++c) g_vga[g_row * VGA_COLS + c] = mk_cell(' ');
    move_hw_cursor();
}

static void be_putchar(char ch) {
    if (ch == '\n') {
        g_col = 0;
        g_row++;
        scroll_if_needed();
        move_hw_cursor();
        return;
    }
    if (ch == '\r') {
        g_col = 0;
        move_hw_cursor();
        return;
    }
    if (ch == '\b') {
        if (g_col > 0) g_col--;
        move_hw_cursor();
        return;
    }

    g_vga[g_row * VGA_COLS + g_col] = mk_cell(ch);
    g_col++;
    if (g_col >= VGA_COLS) {
        g_col = 0;
        g_row++;
        scroll_if_needed();
    }
    move_hw_cursor();
}

const console_backend_t VGA_CONSOLE = {
    be_init,
    be_set_color,
    be_clear,
    be_putchar,
    be_move_cursor,
    be_get_cursor,
    be_get_cols,
    be_get_rows,
    be_erase_in_line
};
