#pragma once

#include <stdint.h>
#include "console_backend.h"

extern const console_backend_t FB_CONSOLE;

void fb_console_present(void);
void fb_console_tick(uint32_t ticks);
void fb_console_set_cursor_enabled(int enabled);
void fb_console_get_cursor(int *col, int *row);
int fb_console_get_cols(void);
int fb_console_get_rows(void);
void fb_console_set_present_enabled(int enabled);
