#ifndef SYS_MEMINFO_H
#define SYS_MEMINFO_H

#include <stdint.h>

void meminfo_init(uint32_t magic, void *mb_info);
uint64_t meminfo_total_bytes(void);
uint64_t meminfo_free_bytes(void);
uint64_t meminfo_used_bytes(void);

#endif
