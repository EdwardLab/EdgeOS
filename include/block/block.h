#ifndef BLOCK_BLOCK_H
#define BLOCK_BLOCK_H

#include <stdint.h>

#define BLOCK_NAME_MAX 16
#define BLOCK_MAX_DEVICES 8

typedef struct block_device block_device_t;

#define BLOCK_BATCH_MAX_SECTORS 1024u

typedef struct {
    int (*read_sectors)(block_device_t *dev, uint32_t lba, uint32_t count, void *out);
    int (*write_sectors)(block_device_t *dev, uint32_t lba, uint32_t count, const void *in);
} block_ops_t;

struct block_device {
    int present;
    char name[BLOCK_NAME_MAX];
    uint32_t sector_size;
    uint32_t sector_count;
    uint32_t start_lba;
    void *ctx;
    block_ops_t ops;
};

void block_init(void);
int block_register(const char *name, uint32_t sector_size, uint32_t sector_count, uint32_t start_lba, void *ctx, block_ops_t ops);
block_device_t *block_get(int idx);
block_device_t *block_find(const char *name);
int block_count(void);
int block_read_sectors(block_device_t *dev, uint32_t lba, uint32_t count, void *out);
int block_write_sectors(block_device_t *dev, uint32_t lba, uint32_t count, const void *in);

#endif
