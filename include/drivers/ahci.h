#ifndef DRIVERS_AHCI_H
#define DRIVERS_AHCI_H

#include <stdint.h>

typedef struct {
    uint64_t addr;
    uint32_t len;
} ahci_sg_t;

int ahci_init(void);
int ahci_present(void);
uint32_t ahci_sector_count(void);
int ahci_read(uint32_t lba, uint32_t sector_count, void *buf);
int ahci_write(uint32_t lba, uint32_t sector_count, const void *buf);
int ahci_read_sg(uint32_t lba, uint32_t sector_count, const ahci_sg_t *sg, uint32_t sg_count);
int ahci_write_sg(uint32_t lba, uint32_t sector_count, const ahci_sg_t *sg, uint32_t sg_count);

#endif
