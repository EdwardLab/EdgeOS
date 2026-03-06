#ifndef DRIVERS_ATA_H
#define DRIVERS_ATA_H

#include <stdint.h>

int ata_init_primary_master(void);
int ata_read28(uint32_t lba, uint8_t sector_count, void *buf);
int ata_write28(uint32_t lba, uint8_t sector_count, const void *buf);
int ata_present(void);
uint32_t ata_sector_count(void);

#endif
