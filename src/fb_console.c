#include "fb_console.h"
#include "fb.h"
#include "font8x8_basic.h"
#include "string.h"

static uint32_t vga_rgb(uint32_t idx) {
    static const uint32_t lut[16] = {
        0xFF000000,0xFF0000AA,0xFF00AA00,0xFF00AAAA,0xFFAA0000,0xFFAA00AA,0xFFAA5500,0xFFAAAAAA,
        0xFF555555,0xFF5555FF,0xFF55FF55,0xFF55FFFF,0xFFFF5555,0xFFFF55FF,0xFFFFFF55,0xFFFFFFFF
    };
    return lut[idx & 0x0F];
}

static uint32_t fg_idx = 15, bg_idx = 0;
static int cur_col, cur_row, cols, rows;
static int cursor_enabled, cursor_visible;
static uint32_t last_blink_tick;
static int dirty;
static int present_enabled = 1;

enum { GLYPH_W = 8, GLYPH_H = 8, COL_SPACE = 1, LINE_SPACE = 2 };
static int cell_w(void){ return GLYPH_W + COL_SPACE; }
static int cell_h(void){ return GLYPH_H + LINE_SPACE; }
static int col_x(int c){ return c * cell_w(); }
static int row_y(int r){ return r * cell_h(); }

static void draw_cell(int col, int row, unsigned char ch, uint32_t fg, uint32_t bg) {
    int x = col_x(col), y = row_y(row);
    const uint8_t *g = (const uint8_t*)font8x8_basic[ch];
    for (int ry = 0; ry < GLYPH_H; ry++) {
        uint8_t bits = g[ry];
        for (int cx = 0; cx < GLYPH_W; cx++) fb_putpixel(x + cx, y + ry, (bits & (1u << cx)) ? fg : bg);
    }
    for (int ry = GLYPH_H; ry < cell_h(); ry++) for (int cx = 0; cx < GLYPH_W; cx++) fb_putpixel(x + cx, y + ry, bg);
    for (int ry = 0; ry < cell_h(); ry++) fb_putpixel(x + GLYPH_W, y + ry, bg);
}

static void draw_cursor(void) {
    if (!cursor_enabled || !cursor_visible) return;
    int x = col_x(cur_col), y = row_y(cur_row) + GLYPH_H - 1;
    for (int cx = 0; cx < GLYPH_W; cx++) fb_putpixel(x + cx, y, vga_rgb(fg_idx));
}

static void erase_cursor(void) {
    if (!cursor_enabled || !cursor_visible) return;
    int x = col_x(cur_col), y = row_y(cur_row) + GLYPH_H - 1;
    for (int cx = 0; cx < GLYPH_W; cx++) fb_putpixel(x + cx, y, vga_rgb(bg_idx));
}

static void scroll_up_one_row(void) {
    uint8_t *buf = fb_get_draw_buffer();
    uint32_t dy = (uint32_t)cell_h() * fb.pitch;
    uint32_t total = (uint32_t)rows * cell_h() * fb.pitch;
    if (!buf || total <= dy) return;
    memmove(buf, buf + dy, total - dy);
    memset(buf + (total - dy), 0, dy);
}

static void newline(void) {
    cur_col = 0;
    cur_row++;
    if (cur_row >= rows) {
        scroll_up_one_row();
        cur_row = rows - 1;
    }
}

static void be_init(uint32_t fg, uint32_t bg) {
    (void)fb_enable_backbuffer(); /* Keep console state off-screen so fbdev apps can take over display. */
    fg_idx = fg & 0x0F; bg_idx = bg & 0x0F;
    cur_col = cur_row = 0;
    cols = (int)(fb.width / cell_w()); if (cols < 1) cols = 1;
    rows = (int)(fb.height / cell_h()); if (rows < 1) rows = 1;
    cursor_enabled = 1;
    cursor_visible = 1;
    last_blink_tick = 0;
    fb_clear(vga_rgb(bg_idx));
    draw_cursor();
    dirty = 1;
}

static void be_set_color(uint32_t fg, uint32_t bg) { fg_idx = fg & 0x0F; bg_idx = bg & 0x0F; }

static void be_clear(void) {
    erase_cursor();
    cur_col = cur_row = 0;
    fb_clear(vga_rgb(bg_idx));
    draw_cursor();
    dirty = 1;
}

static void be_move_cursor(int x, int y) {
    erase_cursor();
    if (x >= 0 && x < cols) cur_col = x;
    if (y >= 0 && y < rows) cur_row = y;
    draw_cursor();
    dirty = 1;
}

static void be_get_cursor(int *x, int *y) {
    if (x) *x = cur_col;
    if (y) *y = cur_row;
}

static int be_get_cols(void) { return cols; }
static int be_get_rows(void) { return rows; }

static void be_erase_in_line(int mode) {
    int start = 0;
    int end = cols - 1;
    if (mode == 0) start = cur_col;
    else if (mode == 1) end = cur_col;
    else if (mode == 2) { start = 0; end = cols - 1; }
    else return;
    if (start < 0) start = 0;
    if (end >= cols) end = cols - 1;
    if (start > end) return;

    erase_cursor();
    for (int c = start; c <= end; ++c) {
        draw_cell(c, cur_row, ' ', vga_rgb(fg_idx), vga_rgb(bg_idx));
    }
    draw_cursor();
    dirty = 1;
}

static void be_putchar(char ch) {
    erase_cursor();
    if (ch == '\n') newline();
    else if (ch == '\r') cur_col = 0;
    else if (ch == '\b') {
        if (cur_col > 0) cur_col--;
    } else {
        draw_cell(cur_col, cur_row, (unsigned char)ch, vga_rgb(fg_idx), vga_rgb(bg_idx));
        cur_col++;
        if (cur_col >= cols) newline();
    }
    draw_cursor();
    dirty = 1;
}

const console_backend_t FB_CONSOLE = {
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

void fb_console_present(void) {
    if (!present_enabled) return;
    if (dirty) { fb_present(); dirty = 0; }
}

void fb_console_tick(uint32_t ticks) {
    if (!cursor_enabled) return;
    if (ticks - last_blink_tick >= 9) {
        last_blink_tick = ticks;
        if (cursor_visible) { erase_cursor(); cursor_visible = 0; }
        else { cursor_visible = 1; draw_cursor(); }
        dirty = 1;
    }
}

void fb_console_set_cursor_enabled(int enabled) {
    if (cursor_visible) erase_cursor();
    cursor_enabled = enabled ? 1 : 0;
    cursor_visible = cursor_enabled;
    if (cursor_visible) draw_cursor();
    dirty = 1;
}

void fb_console_get_cursor(int *col, int *row) { if (col) *col = cur_col; if (row) *row = cur_row; }
int fb_console_get_cols(void) { return cols; }
int fb_console_get_rows(void) { return rows; }
void fb_console_set_present_enabled(int enabled) {
    present_enabled = enabled ? 1 : 0;
    if (present_enabled) fb_console_present();
}
