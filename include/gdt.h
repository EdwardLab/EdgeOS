#ifndef GDT_H
#define GDT_H

#include <stdint.h>

void gdt_init(void);
void gdt_set_tss_rsp0(uint64_t rsp0);

#endif
