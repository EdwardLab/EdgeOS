#include "dev/dev.h"
#include "block/block.h"
#include "drivers/ahci.h"
#include "drivers/ata.h"
#include "sys/bootlog.h"
#include "multiboot.h"
#include "string.h"
#include "stdio.h"

typedef struct {
    uint8_t *base;
    uint32_t size;
    uint32_t offset;
} ram_block_ctx_t;

#define MAX_RAMDISK_BYTES (128u * 1024u * 1024u)

#pragma pack(push,1)
typedef struct {
    uint8_t status;
    uint8_t chs_start[3];
    uint8_t type;
    uint8_t chs_end[3];
    uint32_t lba_start;
    uint32_t sectors;
} mbr_part_t;
#pragma pack(pop)


#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289u

typedef struct {
    uint32_t type;
    uint32_t size;
} mb2_tag_t;

typedef struct {
    mb2_tag_t tag;
    uint32_t mod_start;
    uint32_t mod_end;
    char cmdline[];
} mb2_tag_module_t;

static ram_block_ctx_t g_ram_ctx[5];
static char g_dev_names[BLOCK_MAX_DEVICES][16];
static int g_has_module_ramdisk;
static uint8_t g_ramdisk_copy[MAX_RAMDISK_BYTES] __attribute__((aligned(4096)));
static int ram_read(block_device_t *dev, uint32_t lba, uint32_t count, void *out);
static int ram_write(block_device_t *dev, uint32_t lba, uint32_t count, const void *in);
static int dev_module_has_ext_superblock(uint32_t mod_start, uint32_t mod_end);
static int dev_module_cmdline_score(const mb2_tag_module_t *m);

static int dev_register_module_ramdisk(uint32_t mod_start, uint32_t mod_end) {
    extern char _kernel_start;
    extern char _kernel_end;
    if (mod_end <= mod_start) return 0;
    g_ram_ctx[0].size = mod_end - mod_start;
    if (g_ram_ctx[0].size > MAX_RAMDISK_BYTES) {
        /* Some LiveCD images are larger than the static copy buffer.
         * Use the GRUB-provided module memory directly. */
        g_ram_ctx[0].base = (uint8_t *)(uintptr_t)mod_start;
        g_ram_ctx[0].offset = 0;
        printf("[dev] module too large for ramdisk copy: %u bytes, using in-place module memory\n",
               g_ram_ctx[0].size);
    } else {
        memcpy(g_ramdisk_copy, (const void *)(uintptr_t)mod_start, g_ram_ctx[0].size);
        g_ram_ctx[0].base = g_ramdisk_copy;
        g_ram_ctx[0].offset = 0;
    }
    block_ops_t ops = {ram_read, ram_write};
    if (block_register("ram0", 512, g_ram_ctx[0].size / 512, 0, &g_ram_ctx[0], ops) < 0) return 0;
    g_has_module_ramdisk = 1;
    printf("[dev] multiboot module rootfs copied and registered as /dev/ram0 (%u bytes)\n", g_ram_ctx[0].size);
    printf("[dev] module range=0x%x..0x%x kernel=0x%x..0x%x\n",
           mod_start, mod_end,
           (uint32_t)(uintptr_t)&_kernel_start, (uint32_t)(uintptr_t)&_kernel_end);
    (void)block_register("sda", 512, g_ram_ctx[0].size / 512, 0, &g_ram_ctx[0], ops);
    return 1;
}

static int dev_try_scan_mb2_modules(void *mb_info) {
    if (!mb_info) return 0;
    uint8_t *base = (uint8_t *)mb_info;
    uint32_t total_size = *(uint32_t *)base;
    if (total_size < 16 || total_size > (16u * 1024u * 1024u)) return 0;

    uint8_t *p = base + 8;
    uint8_t *endp = base + total_size;
    mb2_tag_module_t *best = 0;
    int best_score = -1;
    mb2_tag_module_t *first = 0;
    while (p + sizeof(mb2_tag_t) <= endp) {
        mb2_tag_t *tag = (mb2_tag_t *)p;
        if (tag->type == 0) break;
        if (tag->size < sizeof(mb2_tag_t)) break;
        if (tag->type == 3 && tag->size >= sizeof(mb2_tag_module_t)) {
            mb2_tag_module_t *m = (mb2_tag_module_t *)tag;
            int score = 0;
            if (!first) first = m;
            if (dev_module_has_ext_superblock(m->mod_start, m->mod_end)) score += 100;
            score += dev_module_cmdline_score(m);
            if (score > best_score) {
                best = m;
                best_score = score;
            }
        }
        p += (tag->size + 7u) & ~7u;
    }
    if (!best) best = first;
    if (best) return dev_register_module_ramdisk(best->mod_start, best->mod_end);
    return 0;
}

static int dev_module_has_ext_superblock(uint32_t mod_start, uint32_t mod_end) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)mod_start;
    uint32_t size;
    if (mod_end <= mod_start) return 0;
    size = mod_end - mod_start;
    /* ext2/ext4 superblock magic at offset 1024 + 56 */
    if (size < 1082) return 0;
    return p[1080] == 0x53 && p[1081] == 0xEF;
}

static int dev_module_cmdline_score(const mb2_tag_module_t *m) {
    const char *cmd;
    if (!m || m->tag.size <= sizeof(mb2_tag_module_t)) return 0;
    cmd = m->cmdline;
    if (!cmd[0]) return 0;
    if (strstr(cmd, "rootfs")) return 20;
    if (strstr(cmd, ".img")) return 10;
    return 0;
}

static int ram_read(block_device_t *dev, uint32_t lba, uint32_t count, void *out) {
    ram_block_ctx_t *c = (ram_block_ctx_t *)dev->ctx;
    uint32_t bytes = count * dev->sector_size;
    uint32_t off = (c->offset + lba) * dev->sector_size;
    if (off + bytes > c->size) return -1;
    memcpy(out, c->base + off, bytes);
    return 0;
}

static int ram_write(block_device_t *dev, uint32_t lba, uint32_t count, const void *in) {
    ram_block_ctx_t *c = (ram_block_ctx_t *)dev->ctx;
    uint32_t bytes = count * dev->sector_size;
    uint32_t off = (c->offset + lba) * dev->sector_size;
    if (off + bytes > c->size) return -1;
    memcpy(c->base + off, in, bytes);
    return 0;
}

static int ata_read_ops(block_device_t *dev, uint32_t lba, uint32_t count, void *out) {
    if (!dev) return -1;
    return ata_read28(dev->start_lba + lba, (uint8_t)count, out);
}

static int ata_write_ops(block_device_t *dev, uint32_t lba, uint32_t count, const void *in) {
    if (!dev) return -1;
    return ata_write28(dev->start_lba + lba, (uint8_t)count, in);
}

static int ahci_read_ops(block_device_t *dev, uint32_t lba, uint32_t count, void *out) {
    if (!dev) return -1;
    return ahci_read(dev->start_lba + lba, count, out);
}

static int ahci_write_ops(block_device_t *dev, uint32_t lba, uint32_t count, const void *in) {
    if (!dev) return -1;
    return ahci_write(dev->start_lba + lba, count, in);
}

static void dev_register_partitions(block_device_t *disk, const char *prefix, ram_block_ctx_t *ctx) {
    uint8_t sector[512];
    if (block_read_sectors(disk, 0, 1, sector) < 0) return;
    if (sector[510] != 0x55 || sector[511] != 0xAA) return;
    mbr_part_t *parts = (mbr_part_t *)(sector + 446);
    for (int i = 0; i < 4; ++i) {
        if (parts[i].sectors == 0) continue;
        char name[16];
        name[0]=prefix[0]; name[1]=prefix[1]; name[2]=prefix[2]; name[3]=0;
        int nlen=3; if (prefix[3]) {name[3]=prefix[3]; name[4]=0; nlen=4;}
        name[nlen]=(char)('0'+(i+1)); name[nlen+1]=0;
        block_ops_t ops = disk->ops;
        void *part_ctx = disk->ctx;
        uint32_t start_lba = disk->start_lba + parts[i].lba_start;
        if (ctx) {
            ctx[i + 1] = *ctx;
            ctx[i + 1].offset = parts[i].lba_start;
            part_ctx = &ctx[i + 1];
            start_lba = parts[i].lba_start;
        }
        block_register(name, disk->sector_size, parts[i].sectors, start_lba, part_ctx, ops);
    }
}

int dev_has_valid_mbr(const char *disk_name) {
    uint8_t sector[512];
    block_device_t *disk;
    if (!disk_name) return 0;
    disk = block_find(disk_name[0] == '/' ? disk_name + 5 : disk_name);
    if (!disk) return 0;
    if (block_read_sectors(disk, 0, 1, sector) < 0) return 0;
    return sector[510] == 0x55 && sector[511] == 0xAA;
}

void dev_init(uint32_t magic, void *mb_info) {
    block_init();
    g_has_module_ramdisk = 0;

    if (magic == MULTIBOOT_BOOTLOADER_MAGIC) {
        bootlog_stage("Scanning multiboot1 modules for ramdisk");
        multiboot_info_t *mb = (multiboot_info_t *)mb_info;
        if (mb && (mb->flags & MULTIBOOT_INFO_MODS) && mb->mods_count > 0) {
            multiboot_module_t *mods = (multiboot_module_t *)(uintptr_t)mb->mods_addr;
            (void)dev_register_module_ramdisk(mods[0].mod_start, mods[0].mod_end);
        }
    }

    if (!g_has_module_ramdisk && magic == MULTIBOOT2_BOOTLOADER_MAGIC && mb_info) {
        bootlog_stage("Scanning multiboot2 modules for ramdisk");
        (void)dev_try_scan_mb2_modules(mb_info);
    }

    if (!g_has_module_ramdisk && mb_info) {
        bootlog_stage("Fallback scan for multiboot2 module ramdisk");
        (void)dev_try_scan_mb2_modules(mb_info);
    }

    if (!g_has_module_ramdisk) bootlog_stage("Waiting for AHCI SATA identify");
    if (!g_has_module_ramdisk && ahci_init() == 0 && ahci_present()) {
        block_ops_t ops = {ahci_read_ops, ahci_write_ops};
        block_register("sda", 512, ahci_sector_count(), 0, 0, ops);
        dev_register_partitions(block_find("sda"), "sda", 0);
    }

    if (!g_has_module_ramdisk && !block_find("sda")) bootlog_stage("AHCI unavailable, fallback to ATA primary master identify");
    if (!g_has_module_ramdisk && !block_find("sda") && ata_init_primary_master() == 0 && ata_present()) {
        block_ops_t ops = {ata_read_ops, ata_write_ops};
        block_register("sda", 512, ata_sector_count(), 0, 0, ops);
        dev_register_partitions(block_find("sda"), "sda", 0);
    }

    for (int i = 0; i < block_count(); ++i) {
        block_device_t *d = block_get(i);
        strcpy(g_dev_names[i], "/dev/");
        strcat(g_dev_names[i], d->name);
    }
}

const char *dev_get_name(int index) {
    if (index < 0 || index >= block_count()) return 0;
    return g_dev_names[index];
}
