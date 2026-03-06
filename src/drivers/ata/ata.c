#include "drivers/ata.h"
#include "io_ports.h"
#include "sys/bootlog.h"

#define ATA_IO 0x1F0
#define ATA_CTRL 0x3F6

static int g_present;
static uint32_t g_sector_count;

static inline void ata_delay(void) {
    inportb(ATA_IO + 7);
    inportb(ATA_IO + 7);
    inportb(ATA_IO + 7);
    inportb(ATA_IO + 7);
}

static int ata_wait_drq(void) {
    ata_delay();
    for (int i = 0; i < 1000000; ++i) {
        uint8_t st = inportb(ATA_IO + 7);
        if (!(st & 0x80) && (st & 0x08)) return 0;
        if (st & 0x01) return -1;
    }
    return -1;
}

static int ata_wait_not_busy(void) {
    ata_delay();
    for (int i = 0; i < 1000000; ++i) {
        uint8_t st = inportb(ATA_IO + 7);
        if (!(st & 0x80)) return (st & 0x01) ? -1 : 0;
    }
    return -1;
}


int ata_present(void) { return g_present; }
uint32_t ata_sector_count(void) { return g_sector_count; }

int ata_init_primary_master(void) {
    bootlog_stage("Probing ATA primary master");
    outportb(ATA_CTRL, 0x02);
    outportb(ATA_IO + 6, 0xA0);
    ata_delay();
    outportb(ATA_IO + 2, 0);
    outportb(ATA_IO + 3, 0);
    outportb(ATA_IO + 4, 0);
    outportb(ATA_IO + 5, 0);
    outportb(ATA_IO + 7, 0xEC);

    uint8_t st = inportb(ATA_IO + 7);
    if (st == 0) {
        g_present = 0;
        return -1;
    }
    if (ata_wait_not_busy() < 0) {
        g_present = 0;
        return -1;
    }
    if (inportb(ATA_IO + 4) || inportb(ATA_IO + 5)) {
        g_present = 0;
        return -1;
    }
    if (ata_wait_drq() < 0) {
        g_present = 0;
        return -1;
    }

    uint16_t ident[256];
    for (int i = 0; i < 256; ++i) ident[i] = inports(ATA_IO);
    g_sector_count = ((uint32_t)ident[61] << 16) | ident[60];
    g_present = 1;
    bootlog_stage("ATA primary master ready");
    return 0;
}

int ata_read28(uint32_t lba, uint8_t sector_count, void *buf) {
    if (!g_present || !sector_count) return -1;
    if (ata_wait_not_busy() < 0) return -1;

    outportb(ATA_IO + 6, 0xE0 | ((lba >> 24) & 0x0F));
    outportb(ATA_IO + 2, sector_count);
    outportb(ATA_IO + 3, lba & 0xFF);
    outportb(ATA_IO + 4, (lba >> 8) & 0xFF);
    outportb(ATA_IO + 5, (lba >> 16) & 0xFF);
    outportb(ATA_IO + 7, 0x20);

    uint16_t *ptr = (uint16_t *)buf;
    for (uint8_t s = 0; s < sector_count; ++s) {
        if (ata_wait_drq() < 0) return -1;
        for (int i = 0; i < 256; ++i) *ptr++ = inports(ATA_IO);
    }
    return 0;
}

int ata_write28(uint32_t lba, uint8_t sector_count, const void *buf) {
    if (!g_present || !sector_count) return -1;
    if (ata_wait_not_busy() < 0) return -1;

    outportb(ATA_IO + 6, 0xE0 | ((lba >> 24) & 0x0F));
    outportb(ATA_IO + 2, sector_count);
    outportb(ATA_IO + 3, lba & 0xFF);
    outportb(ATA_IO + 4, (lba >> 8) & 0xFF);
    outportb(ATA_IO + 5, (lba >> 16) & 0xFF);
    outportb(ATA_IO + 7, 0x30);

    const uint16_t *ptr = (const uint16_t *)buf;
    for (uint8_t s = 0; s < sector_count; ++s) {
        if (ata_wait_drq() < 0) return -1;
        for (int i = 0; i < 256; ++i) outports(ATA_IO, *ptr++);
        if (ata_wait_not_busy() < 0) return -1;
    }
    return 0;
}