#include "sys/bootlog.h"
#include "sys/boottime.h"
#include "stdio.h"
#include "string.h"

static char g_bootlog_buf[16384];
static int g_bootlog_len;

void bootlog_stage(const char *msg) {
    uint32_t us = boottime_now_us();
    uint32_t sec = us / 1000000u;
    uint32_t frac = us % 1000000u;
    printf("[%5u.%06u] %s\n", sec, frac, msg ? msg : "");

    if (!msg) return;
    int n = strlen(msg);
    if (n <= 0) return;
    if (g_bootlog_len + n + 1 >= (int)sizeof(g_bootlog_buf)) return;
    memcpy(&g_bootlog_buf[g_bootlog_len], msg, (uint32_t)n);
    g_bootlog_len += n;
    g_bootlog_buf[g_bootlog_len++] = '\n';
    g_bootlog_buf[g_bootlog_len] = 0;
}

const char *bootlog_buffer(void) { return g_bootlog_buf; }
int bootlog_buffer_size(void) { return g_bootlog_len; }
