#include "sys/meminfo.h"
#include "multiboot.h"

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289u

static uint64_t g_total;
static uint64_t g_free;
static uint64_t g_used;

extern uint8_t _kernel_start;
extern uint8_t _kernel_end;

struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

struct mb2_tag_mmap {
    struct mb2_tag tag;
    uint32_t entry_size;
    uint32_t entry_version;
    uint8_t entries[];
};

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
} mb2_mmap_entry_t;

static uint64_t overlap_bytes(uint64_t a_start, uint64_t a_len, uint64_t b_start, uint64_t b_len) {
    uint64_t a_end = a_start + a_len;
    uint64_t b_end = b_start + b_len;
    uint64_t lo = a_start > b_start ? a_start : b_start;
    uint64_t hi = a_end < b_end ? a_end : b_end;
    return hi > lo ? (hi - lo) : 0;
}

static void account_available_range(uint64_t addr, uint64_t len) {
    uint64_t kernel_start = (uint64_t)(uintptr_t)&_kernel_start;
    uint64_t kernel_len = (uint64_t)((uintptr_t)&_kernel_end - (uintptr_t)&_kernel_start);
    uint64_t reserved_in_range = overlap_bytes(addr, len, kernel_start, kernel_len);

    g_total += len;
    g_used += reserved_in_range;
}

void meminfo_init(uint32_t magic, void *mb_info) {
    g_total = 0;
    g_free = 0;
    g_used = 0;
    if (!mb_info) return;

    if (magic == MULTIBOOT_BOOTLOADER_MAGIC) {
        multiboot_info_t *mb = (multiboot_info_t *)mb_info;
        if (mb->flags & MULTIBOOT_INFO_MEM_MAP) {
            uint32_t end = mb->mmap_addr + mb->mmap_length;
            for (uint32_t p = mb->mmap_addr; p < end;) {
                multiboot_memory_map_t *e = (multiboot_memory_map_t *)(uintptr_t)p;
                if (e->type == MULTIBOOT_MEMORY_AVAILABLE) account_available_range((uint64_t)e->addr, (uint64_t)e->len);
                p += e->size + sizeof(e->size);
            }
        } else if (mb->flags & MULTIBOOT_INFO_MEMORY) {
            g_total = ((uint64_t)mb->mem_lower + mb->mem_upper) * 1024ull;
            g_used = overlap_bytes(0, g_total, (uint64_t)(uintptr_t)&_kernel_start, (uint64_t)((uintptr_t)&_kernel_end - (uintptr_t)&_kernel_start));
        }
        if (g_used > g_total) g_used = g_total;
        g_free = g_total - g_used;
        return;
    }

    if (magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        uint8_t *base = (uint8_t *)mb_info;
        uint32_t total_size = *(uint32_t *)base;
        uint8_t *p = base + 8;
        uint8_t *end = base + total_size;
        while (p + sizeof(struct mb2_tag) <= end) {
            struct mb2_tag *tag = (struct mb2_tag *)p;
            if (tag->type == 0) break;
            if (tag->type == 6 && tag->size >= sizeof(struct mb2_tag_mmap)) {
                struct mb2_tag_mmap *m = (struct mb2_tag_mmap *)tag;
                uint8_t *mp = m->entries;
                uint8_t *me = ((uint8_t *)tag) + tag->size;
                while (mp + m->entry_size <= me && m->entry_size >= sizeof(mb2_mmap_entry_t)) {
                    mb2_mmap_entry_t *e = (mb2_mmap_entry_t *)mp;
                    if (e->type == MULTIBOOT_MEMORY_AVAILABLE) account_available_range(e->addr, e->len);
                    mp += m->entry_size;
                }
            }
            p += (tag->size + 7u) & ~7u;
        }
        if (g_used > g_total) g_used = g_total;
        g_free = g_total - g_used;
    }
}

uint64_t meminfo_total_bytes(void) { return g_total; }
uint64_t meminfo_free_bytes(void) { return g_free; }
uint64_t meminfo_used_bytes(void) { return g_used; }
