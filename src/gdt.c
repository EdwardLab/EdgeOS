#include "gdt.h"

#include "types.h"

extern void load_gdt(uint64 gdt_ptr);
extern void load_tss(uint16 tss_sel);

typedef struct {
    uint16 limit;
    uint64 base;
} __attribute__((packed)) gdt_ptr_t;

typedef struct {
    uint32 reserved0;
    uint64 rsp0;
    uint64 rsp1;
    uint64 rsp2;
    uint64 reserved1;
    uint64 ist1;
    uint64 ist2;
    uint64 ist3;
    uint64 ist4;
    uint64 ist5;
    uint64 ist6;
    uint64 ist7;
    uint64 reserved2;
    uint16 reserved3;
    uint16 iomap_base;
} __attribute__((packed)) tss64_t;

static uint64 g_gdt[7] __attribute__((aligned(16)));
static tss64_t g_tss __attribute__((aligned(16)));
static gdt_ptr_t g_gdt_ptr;

static void gdt_set_tss_desc(int idx, uint64 base, uint32 limit) {
    uint64 lo = 0;
    uint64 hi = 0;

    lo |= (limit & 0xFFFFULL);
    lo |= (base & 0xFFFFFFULL) << 16;
    lo |= 0x89ULL << 40;
    lo |= ((limit >> 16) & 0xFULL) << 48;
    lo |= ((base >> 24) & 0xFFULL) << 56;

    hi |= (base >> 32) & 0xFFFFFFFFULL;

    g_gdt[idx] = lo;
    g_gdt[idx + 1] = hi;
}

__attribute__((used)) void gdt_set_tss_rsp0(uint64_t rsp0) {
    g_tss.rsp0 = rsp0;
}

void gdt_init(void) {
    g_gdt[0] = 0;
    g_gdt[1] = 0x00AF9A000000FFFFULL;
    g_gdt[2] = 0x00AF92000000FFFFULL;
    g_gdt[3] = 0x00AFFA000000FFFFULL;
    g_gdt[4] = 0x00AFF2000000FFFFULL;

    for (uint32 i = 0; i < sizeof(g_tss); ++i) ((uint8 *)&g_tss)[i] = 0;
    g_tss.iomap_base = sizeof(g_tss);

    gdt_set_tss_desc(5, (uint64)&g_tss, sizeof(g_tss) - 1);

    g_gdt_ptr.base = (uint64)&g_gdt[0];
    g_gdt_ptr.limit = sizeof(g_gdt) - 1;

    load_gdt((uint64)&g_gdt_ptr);
    load_tss(0x28);
}
