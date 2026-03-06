#ifndef SYS_BOOTTIME_H
#define SYS_BOOTTIME_H

#include <stdint.h>

void boottime_init(void);
uint32_t boottime_now_us(void);
uint64_t boottime_monotonic_us(void);
uint64_t boottime_realtime_us(void);

#endif
