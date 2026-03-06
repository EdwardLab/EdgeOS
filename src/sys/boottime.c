#include "sys/boottime.h"
#include "io_ports.h"

#define PIT_CMD_PORT 0x43
#define PIT_CH0_PORT 0x40
#define PIT_HZ_NUM 1193182ull
static uint64_t g_boot_realtime_us;
static uint64_t g_last_monotonic_us;
static uint16_t g_last_pit_counter;
static uint64_t g_fallback_pit_counts;
static uint64_t g_boot_tsc;
static uint64_t g_tsc_hz;
static int g_tsc_ready;

static uint64_t rdtsc_read(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static uint16_t pit_read_ch0_counter(void) {
    outportb(PIT_CMD_PORT, 0x00);
    uint8_t lo = inportb(PIT_CH0_PORT);
    uint8_t hi = inportb(PIT_CH0_PORT);
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

static int rtc_updating(void) {
    outportb(0x70, 0x0A);
    return (inportb(0x71) & 0x80) != 0;
}

static uint8_t rtc_read_reg(uint8_t reg) {
    outportb(0x70, reg);
    return inportb(0x71);
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

static int is_leap(int y) {
    return ((y % 4) == 0 && (y % 100) != 0) || ((y % 400) == 0);
}

static uint64_t ymd_hms_to_unix(int y, int mon, int day, int h, int m, int s) {
    static const int mdays[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    uint64_t days = 0;
    for (int yy = 1970; yy < y; ++yy) days += (uint64_t)(is_leap(yy) ? 366 : 365);
    for (int mm = 1; mm < mon; ++mm) {
        days += (uint64_t)mdays[mm - 1];
        if (mm == 2 && is_leap(y)) days++;
    }
    days += (uint64_t)(day - 1);
    return days * 86400ull + (uint64_t)h * 3600ull + (uint64_t)m * 60ull + (uint64_t)s;
}

static uint64_t rtc_unix_seconds(void) {
    uint8_t sec, min, hour, day, mon, year, regb;
    do { } while (rtc_updating());
    sec = rtc_read_reg(0x00);
    min = rtc_read_reg(0x02);
    hour = rtc_read_reg(0x04);
    day = rtc_read_reg(0x07);
    mon = rtc_read_reg(0x08);
    year = rtc_read_reg(0x09);
    regb = rtc_read_reg(0x0B);

    if ((regb & 0x04) == 0) {
        sec = bcd_to_bin(sec);
        min = bcd_to_bin(min);
        hour = bcd_to_bin(hour & 0x7F);
        day = bcd_to_bin(day);
        mon = bcd_to_bin(mon);
        year = bcd_to_bin(year);
    } else {
        hour &= 0x7F;
    }

    int full_year = 2000 + (int)year;
    if (full_year < 1970) full_year = 1970;
    return ymd_hms_to_unix(full_year, mon ? mon : 1, day ? day : 1, hour, min, sec);
}

static int boottime_calibrate_tsc(void) {
    uint16_t start_pit;
    uint64_t start_tsc;
    uint64_t end_tsc;
    uint32_t elapsed_counts;
    const uint32_t target_counts = 40000u; /* ~33.5ms */
    uint32_t spins = 0;

    start_pit = pit_read_ch0_counter();
    start_tsc = rdtsc_read();

    for (;;) {
        uint16_t now = pit_read_ch0_counter();
        elapsed_counts = (uint32_t)((uint16_t)(start_pit - now));
        if (elapsed_counts >= target_counts) break;
        if (++spins > 10000000u) return -1;
        __asm__ __volatile__("pause");
    }

    end_tsc = rdtsc_read();
    if (end_tsc <= start_tsc || elapsed_counts == 0) return -1;
    g_tsc_hz = ((end_tsc - start_tsc) * PIT_HZ_NUM) / (uint64_t)elapsed_counts;
    if (g_tsc_hz < 1000000ull) return -1;
    return 0;
}

void boottime_init(void) {
    g_last_pit_counter = pit_read_ch0_counter();
    g_fallback_pit_counts = 0;
    g_boot_realtime_us = rtc_unix_seconds() * 1000000ull;
    g_last_monotonic_us = 0;
    g_tsc_ready = 0;
    g_tsc_hz = 0;
    if (boottime_calibrate_tsc() == 0) {
        g_boot_tsc = rdtsc_read();
        g_tsc_ready = 1;
    }
}

uint64_t boottime_monotonic_us(void) {
    uint64_t now_us;
    if (g_tsc_ready && g_tsc_hz > 0) {
        uint64_t now_tsc = rdtsc_read();
        uint64_t delta = now_tsc - g_boot_tsc;
        now_us = (delta * 1000000ull) / g_tsc_hz;
    } else {
        uint16_t pit_now = pit_read_ch0_counter();
        uint16_t delta = (uint16_t)(g_last_pit_counter - pit_now);
        g_last_pit_counter = pit_now;
        g_fallback_pit_counts += (uint64_t)delta;
        now_us = (g_fallback_pit_counts * 1000000ull) / PIT_HZ_NUM;
    }

    if (now_us < g_last_monotonic_us) {
        now_us = g_last_monotonic_us;
    } else {
        g_last_monotonic_us = now_us;
    }
    return now_us;
}

uint64_t boottime_realtime_us(void) {
    return g_boot_realtime_us + boottime_monotonic_us();
}

uint32_t boottime_now_us(void) {
    return (uint32_t)boottime_monotonic_us();
}
