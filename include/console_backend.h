#ifndef CONSOLE_BACKEND_H
#define CONSOLE_BACKEND_H
#include <stdint.h>

typedef struct {
    void (*init)(uint32_t fg, uint32_t bg);
    void (*set_color)(uint32_t fg, uint32_t bg);
    void (*clear)(void);
    void (*putchar)(char ch);
    void (*move_cursor)(int x, int y);
    void (*get_cursor)(int *x, int *y);
    int (*get_cols)(void);
    int (*get_rows)(void);
    void (*erase_in_line)(int mode);
} console_backend_t;

#endif
