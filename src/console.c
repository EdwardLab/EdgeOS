// console.c — backend-agnostic console that forwards to the selected console backend
#include "console.h"
#include "console_backend.h"
#include "keyboard.h"
#include "string.h"
#include <stdint.h>
#include <stdarg.h>

static const console_backend_t* g_be = 0;
static uint32_t g_fg = 0xFFFFFFFF;
static uint32_t g_bg = 0xFF000000;
static uint32_t g_default_fg = 0xFFFFFFFF;
static uint32_t g_default_bg = 0xFF000000;
static int g_esc_state = 0;
static int g_esc_qmark = 0;
static int g_esc_params[8];
static int g_esc_param_count = 0;
static int g_esc_cur = -1;

static int console_cols(void) {
    if (g_be && g_be->get_cols) {
        int c = g_be->get_cols();
        if (c > 0) return c;
    }
    return 80;
}

static int console_rows(void) {
    if (g_be && g_be->get_rows) {
        int r = g_be->get_rows();
        if (r > 0) return r;
    }
    return 25;
}

static void console_get_cursor(int *x, int *y) {
    int cx = 0, cy = 0;
    if (g_be && g_be->get_cursor) g_be->get_cursor(&cx, &cy);
    if (x) *x = cx;
    if (y) *y = cy;
}

static void console_erase_chars_at_cursor(int n) {
    int x, y;
    int cols = console_cols();
    if (n <= 0) return;
    console_get_cursor(&x, &y);
    if (x < 0 || x >= cols) return;
    if (n > cols - x) n = cols - x;
    for (int i = 0; i < n; ++i) {
        console_gotoxy((uint16_t)(x + i), (uint16_t)y);
        if (g_be && g_be->putchar) g_be->putchar(' ');
    }
    console_gotoxy((uint16_t)x, (uint16_t)y);
}

static void console_erase_in_display(int mode) {
    int x, y;
    int rows = console_rows();
    if (rows <= 0) return;
    console_get_cursor(&x, &y);
    if (y < 0) y = 0;
    if (y >= rows) y = rows - 1;

    if (!(g_be && g_be->erase_in_line)) {
        if (mode == 2 || mode == 3) console_clear(g_default_fg, g_default_bg);
        return;
    }

    if (mode == 0) {
        g_be->erase_in_line(0);
        for (int r = y + 1; r < rows; ++r) {
            console_gotoxy(0, (uint16_t)r);
            g_be->erase_in_line(2);
        }
    } else if (mode == 1) {
        for (int r = 0; r < y; ++r) {
            console_gotoxy(0, (uint16_t)r);
            g_be->erase_in_line(2);
        }
        console_gotoxy(0, (uint16_t)y);
        g_be->erase_in_line(1);
    } else if (mode == 2 || mode == 3) {
        for (int r = 0; r < rows; ++r) {
            console_gotoxy(0, (uint16_t)r);
            g_be->erase_in_line(2);
        }
    }
    console_gotoxy((uint16_t)x, (uint16_t)y);
}

static uint32_t ansi_to_vga_idx(int idx) {
    static const uint8_t lut[8] = {
        0, 4, 2, 6, 1, 5, 3, 7
    };
    return (uint32_t)lut[idx & 7];
}

static void console_apply_color(void) {
    if (g_be && g_be->set_color) g_be->set_color(g_fg, g_bg);
}

static void console_emit_raw(char ch) {
    if (g_be && g_be->putchar) g_be->putchar(ch);
}

static void console_esc_reset(void) {
    g_esc_state = 0;
    g_esc_qmark = 0;
    g_esc_param_count = 0;
    g_esc_cur = -1;
}

static int console_esc_param_or(int idx, int defv) {
    if (idx < 0) return defv;
    if (idx < g_esc_param_count) {
        int v = g_esc_params[idx];
        return v == 0 ? defv : v;
    }
    if (idx == g_esc_param_count && g_esc_cur >= 0) {
        int v = g_esc_cur;
        return v == 0 ? defv : v;
    }
    return defv;
}

static int console_esc_first_param(int defv) {
    if (g_esc_param_count > 0) return g_esc_params[0];
    if (g_esc_cur >= 0) return g_esc_cur;
    return defv;
}

static void console_apply_sgr(void) {
    int n = g_esc_param_count;
    if (g_esc_cur >= 0 && n < (int)(sizeof(g_esc_params) / sizeof(g_esc_params[0]))) {
        g_esc_params[n++] = g_esc_cur;
    }
    if (n == 0) {
        g_fg = g_default_fg;
        g_bg = g_default_bg;
        console_apply_color();
        return;
    }
    for (int i = 0; i < n; ++i) {
        int p = g_esc_params[i];
        if (p == 0) {
            g_fg = g_default_fg;
            g_bg = g_default_bg;
        } else if (p == 39) {
            g_fg = g_default_fg;
        } else if (p == 49) {
            g_bg = g_default_bg;
        } else if (p >= 30 && p <= 37) {
            g_fg = ansi_to_vga_idx(p - 30);
        } else if (p >= 40 && p <= 47) {
            g_bg = ansi_to_vga_idx(p - 40);
        } else if (p >= 90 && p <= 97) {
            g_fg = ansi_to_vga_idx(p - 90) | 0x8u;
        } else if (p >= 100 && p <= 107) {
            g_bg = ansi_to_vga_idx(p - 100) | 0x8u;
        }
    }
    console_apply_color();
}

// ===== Backend selection =====
void console_set_backend(const console_backend_t* be) { g_be = be; }

// ===== Basic console control =====
void console_init(uint32_t fore_color, uint32_t back_color) {
    g_fg = fore_color; g_bg = back_color;
    g_default_fg = fore_color;
    g_default_bg = back_color;
    console_esc_reset();
    if (g_be && g_be->init) g_be->init(fore_color, back_color);
}

void console_clear(uint32_t fore_color, uint32_t back_color) {
    g_fg = fore_color; g_bg = back_color;
    g_default_fg = fore_color;
    g_default_bg = back_color;
    console_esc_reset();
    if (g_be && g_be->set_color) g_be->set_color(fore_color, back_color);
    if (g_be && g_be->clear) g_be->clear();
}

void console_putchar(char ch) {
    if (g_esc_state == 0) {
        if ((unsigned char)ch == 0x1B) {
            g_esc_state = 1;
            return;
        }
        console_emit_raw(ch);
        return;
    }
    if (g_esc_state == 1) {
        if (ch == '[') {
            g_esc_state = 2;
            g_esc_qmark = 0;
            g_esc_param_count = 0;
            g_esc_cur = -1;
            return;
        }
        console_emit_raw((char)0x1B);
        console_emit_raw(ch);
        console_esc_reset();
        return;
    }
    if (g_esc_state == 2) {
        if (ch == '?') {
            g_esc_qmark = 1;
            return;
        }
        if (ch >= '0' && ch <= '9') {
            if (g_esc_cur < 0) g_esc_cur = 0;
            g_esc_cur = g_esc_cur * 10 + (ch - '0');
            return;
        }
        if (ch == ';') {
            if (g_esc_param_count < (int)(sizeof(g_esc_params) / sizeof(g_esc_params[0]))) {
                g_esc_params[g_esc_param_count++] = (g_esc_cur < 0) ? 0 : g_esc_cur;
            }
            g_esc_cur = -1;
            return;
        }
        if (ch == 'm') {
            console_apply_sgr();
            console_esc_reset();
            return;
        }
        if (ch == 'H' || ch == 'f') {
            int row = console_esc_param_or(0, 1);
            int col = console_esc_param_or(1, 1);
            if (row < 1) row = 1;
            if (col < 1) col = 1;
            if (row > console_rows()) row = console_rows();
            if (col > console_cols()) col = console_cols();
            console_gotoxy((uint16_t)(col - 1), (uint16_t)(row - 1));
            console_esc_reset();
            return;
        }
        if (ch == 'A' || ch == 'B' || ch == 'C' || ch == 'D') {
            int n = console_esc_first_param(1);
            int x, y;
            int cols = console_cols();
            int rows = console_rows();
            if (n < 1) n = 1;
            console_get_cursor(&x, &y);
            if (ch == 'A') y -= n;
            else if (ch == 'B') y += n;
            else if (ch == 'C') x += n;
            else x -= n;
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (x >= cols) x = cols - 1;
            if (y >= rows) y = rows - 1;
            console_gotoxy((uint16_t)x, (uint16_t)y);
            console_esc_reset();
            return;
        }
        if (ch == 'J') {
            int mode = console_esc_first_param(0);
            console_erase_in_display(mode);
            console_esc_reset();
            return;
        }
        if (ch == 'K') {
            int mode = console_esc_first_param(0);
            if (g_be && g_be->erase_in_line) g_be->erase_in_line(mode);
            console_esc_reset();
            return;
        }
        if (ch == 'P' || ch == 'X') {
            int n = console_esc_first_param(1);
            if (n < 1) n = 1;
            console_erase_chars_at_cursor(n);
            console_esc_reset();
            return;
        }
        if ((ch == 'h' || ch == 'l') && g_esc_qmark && g_esc_cur == 25) {
            console_esc_reset();
            return;
        }
        console_esc_reset();
    }
}

void console_putstr(const char *str) {
    if (!str) return;
    while (*str) console_putchar(*str++);
}

// Optional helpers (no-op if backend doesn't support)
void console_gotoxy(uint16_t x, uint16_t y) {
    if (g_be && g_be->move_cursor) g_be->move_cursor((int)x, (int)y);
}

void console_ungetchar(void) {

    console_putchar('\b');
}

void console_ungetchar_bound(uint8_t n) {
    while (n--) console_ungetchar();
}

// ===== Tiny printf (supports %d %u %x %s with minimal padding) =====

// itoa signature expected by existing string.c: itoa(buf, base, value)
extern void itoa(char *buf, int base, int d);

static void vprintf_core(uint32_t color, const char *format, va_list ap) {
    uint32_t old_fg = g_fg, old_bg = g_bg;
    if (g_be && g_be->set_color) g_be->set_color(color, old_bg);

    int c;
    char buf[32];

    while ((c = *format++) != 0) {
        if (c != '%') {
            console_putchar((char)c);
            continue;
        }

        int pad0 = 0, pad = 0;
        c = *format++;
        if (c == '0') { pad0 = 1; c = *format++; }
        if (c >= '0' && c <= '9') { pad = c - '0'; c = *format++; }

        switch (c) {
        case 'd':
        case 'u':
        case 'x':
        case 'o': {
            int v = va_arg(ap, int);
            memset(buf, 0, sizeof(buf));
            itoa(buf, c, v);
            int len = (int)strlen(buf);
            for (int i = len; i < pad; i++) console_putchar(pad0 ? '0' : ' ');
            console_putstr(buf);
            break;
        }
        case 's': {
            const char *p = va_arg(ap, const char*);
            if (!p) p = "(null)";
            int len = (int)strlen(p);
            for (int i = len; i < pad; i++) console_putchar(pad0 ? '0' : ' ');
            console_putstr(p);
            break;
        }
        case 'c': {
            int ch = va_arg(ap, int);
            console_putchar((char)ch);
            break;
        }
        case '%':
            console_putchar('%');
            break;
        default:
            console_putchar('%');
            console_putchar((char)c);
            break;
        }
    }

    if (g_be && g_be->set_color) g_be->set_color(old_fg, old_bg);
}

void printf(const char *format, ...) {
    va_list ap; va_start(ap, format);
    vprintf_core(0xFFFFFFFF, format, ap);
    va_end(ap);
}

void printf_color(uint32_t color, const char *format, ...) {
    va_list ap; va_start(ap, format);
    vprintf_core(color, format, ap);
    va_end(ap);
}

// ===== Line input helpers =====

void getstr(char *buffer) {
    if (!buffer) return;
    uint32_t i = 0;
    for (;;) {
        char ch = kb_getchar();
        if (ch == '\n') { console_putchar('\n'); buffer[i] = '\0'; return; }
        if (ch == '\b') {
            if (i > 0) { --i; console_ungetchar(); }
            continue;
        }
        buffer[i++] = ch;
        console_putchar(ch);
    }
}

void getstr_bound(char *buffer, uint8_t bound) {
    if (!buffer || bound == 0) return;
    uint32_t i = 0;
    for (;;) {
        char ch = kb_getchar();
        if (ch == '\n') { console_putchar('\n'); buffer[i] = '\0'; return; }
        if (ch == '\b') {
            if (i > 0) { --i; console_ungetchar(); }
            continue;
        }
        if (i + 1 < bound) {   // 留一个给 '\0'
            buffer[i++] = ch;
            console_putchar(ch);
        }
    }
}

// These were used somewhere; return dummy (backend tracks cursor internally)
uint8_t get_cursor_x(void) {
    int x = 0;
    console_get_cursor(&x, 0);
    if (x < 0) x = 0;
    if (x > 255) x = 255;
    return (uint8_t)x;
}

uint8_t get_cursor_y(void) {
    int y = 0;
    console_get_cursor(0, &y);
    if (y < 0) y = 0;
    if (y > 255) y = 255;
    return (uint8_t)y;
}
