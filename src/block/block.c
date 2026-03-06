#include "block/block.h"
#include "string.h"

static block_device_t g_block_devices[BLOCK_MAX_DEVICES];
static int g_block_count;

void block_init(void) {
    memset(g_block_devices, 0, sizeof(g_block_devices));
    g_block_count = 0;
}

int block_register(const char *name, uint32_t sector_size, uint32_t sector_count, uint32_t start_lba, void *ctx, block_ops_t ops) {
    if (g_block_count >= BLOCK_MAX_DEVICES) return -1;
    block_device_t *d = &g_block_devices[g_block_count];
    memset(d, 0, sizeof(*d));
    d->present = 1;
    d->sector_size = sector_size;
    d->sector_count = sector_count;
    d->start_lba = start_lba;
    d->ctx = ctx;
    d->ops = ops;
    if (name) {
        strncpy(d->name, name, BLOCK_NAME_MAX - 1);
        d->name[BLOCK_NAME_MAX - 1] = 0;
    }
    return g_block_count++;
}

block_device_t *block_get(int idx) {
    if (idx < 0 || idx >= g_block_count) return 0;
    return &g_block_devices[idx];
}

block_device_t *block_find(const char *name) {
    for (int i = 0; i < g_block_count; ++i) {
        if (strcmp(g_block_devices[i].name, (char *)name) == 0) return &g_block_devices[i];
    }
    return 0;
}

int block_count(void) { return g_block_count; }

int block_read_sectors(block_device_t *dev, uint32_t lba, uint32_t count, void *out) {
    uint32_t done = 0;
    uint8_t *dst = (uint8_t *)out;
    uint32_t max_batch = BLOCK_BATCH_MAX_SECTORS;
    if (!dev || !out || !dev->ops.read_sectors || dev->sector_size == 0) return -1;
    if (count == 0) return 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > max_batch) chunk = max_batch;
        if (dev->ops.read_sectors(dev, lba + done, chunk, dst + done * dev->sector_size) == 0) {
            done += chunk;
            continue;
        }
        if (chunk <= 1) return -1;
        max_batch = chunk / 2;
        if (max_batch == 0) max_batch = 1;
    }
    return 0;
}

int block_write_sectors(block_device_t *dev, uint32_t lba, uint32_t count, const void *in) {
    uint32_t done = 0;
    const uint8_t *src = (const uint8_t *)in;
    uint32_t max_batch = BLOCK_BATCH_MAX_SECTORS;
    if (!dev || !in || !dev->ops.write_sectors || dev->sector_size == 0) return -1;
    if (count == 0) return 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > max_batch) chunk = max_batch;
        if (dev->ops.write_sectors(dev, lba + done, chunk, src + done * dev->sector_size) == 0) {
            done += chunk;
            continue;
        }
        if (chunk <= 1) return -1;
        max_batch = chunk / 2;
        if (max_batch == 0) max_batch = 1;
    }
    return 0;
}
