#include "drivers/ahci.h"

#include "8259_pic.h"
#include "io_ports.h"
#include "isr.h"
#include "stdio.h"
#include "string.h"
#include "sys/bootlog.h"

#define AHCI_PCI_CLASS_MASS_STORAGE 0x01u
#define AHCI_PCI_SUBCLASS_SATA 0x06u
#define AHCI_PCI_PROGIF_AHCI 0x01u

#define AHCI_PCI_CFG_ADDR 0xCF8u
#define AHCI_PCI_CFG_DATA 0xCFCu

#define AHCI_PxCMD_ST   (1u << 0)
#define AHCI_PxCMD_FRE  (1u << 4)
#define AHCI_PxCMD_FR   (1u << 14)
#define AHCI_PxCMD_CR   (1u << 15)

#define AHCI_PxTFD_BSY  (1u << 7)
#define AHCI_PxTFD_DRQ  (1u << 3)

#define AHCI_PxIS_TFES  (1u << 30)

#define AHCI_HBA_GHC_IE (1u << 1)
#define AHCI_HBA_GHC_AE (1u << 31)

#define AHCI_HBA_PORT_DET_PRESENT 3u
#define AHCI_HBA_PORT_IPM_ACTIVE  1u

#define AHCI_SIG_ATA   0x00000101u

#define AHCI_CMD_IDENTIFY_DEVICE   0xECu
#define AHCI_CMD_READ_DMA_EXT      0x25u
#define AHCI_CMD_WRITE_DMA_EXT     0x35u
#define AHCI_CMD_READ_FPDMA_QUEUED 0x60u
#define AHCI_CMD_WRITE_FPDMA_QUEUED 0x61u

#define AHCI_FIS_TYPE_REG_H2D 0x27u

#define AHCI_MAX_PORTS 32
#define AHCI_MAX_CMDS  32
#define AHCI_MAX_PRDT  16
#define AHCI_MAX_CMD_SECTORS 65535u
#define AHCI_PRDT_MAX_BYTES  (4u * 1024u * 1024u)

typedef volatile struct {
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t rsv0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t rsv1[11];
    uint32_t vendor[4];
} hba_port_t;

typedef volatile struct {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
    uint8_t  rsv[0xA0 - 0x2C];
    uint8_t  vendor[0x100 - 0xA0];
    hba_port_t ports[AHCI_MAX_PORTS];
} hba_mem_t;

typedef struct {
    uint8_t  cfl:5;
    uint8_t  a:1;
    uint8_t  w:1;
    uint8_t  p:1;
    uint8_t  r:1;
    uint8_t  b:1;
    uint8_t  c:1;
    uint8_t  rsv0:1;
    uint8_t  pmp:4;
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv1[4];
} hba_cmd_header_t;

typedef struct {
    uint8_t  fis_type;
    uint8_t  pmport:4;
    uint8_t  rsv0:3;
    uint8_t  c:1;
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0;
    uint8_t  lba1;
    uint8_t  lba2;
    uint8_t  device;
    uint8_t  lba3;
    uint8_t  lba4;
    uint8_t  lba5;
    uint8_t  featureh;
    uint8_t  countl;
    uint8_t  counth;
    uint8_t  icc;
    uint8_t  control;
    uint8_t  rsv1[4];
} fis_reg_h2d_t;

typedef struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv0;
    uint32_t dbc:22;
    uint32_t rsv1:9;
    uint32_t i:1;
} hba_prdt_entry_t;

typedef struct {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    hba_prdt_entry_t prdt_entry[AHCI_MAX_PRDT];
} hba_cmd_tbl_t;

typedef struct {
    int present;
    int port_no;
    hba_port_t *port;
    uint32_t sectors;
    uint32_t queue_depth;
    int ncq_supported;
    int irq_enabled;
    uint8_t irq_line;
    volatile uint32_t outstanding_mask;
    volatile uint32_t slot_tag[AHCI_MAX_CMDS];
    volatile uint32_t slot_done_tag[AHCI_MAX_CMDS];
    volatile int slot_error[AHCI_MAX_CMDS];
} ahci_disk_t;

typedef struct {
    const ahci_sg_t *sg;
    uint32_t sg_count;
    uint32_t index;
    uint32_t off;
} ahci_sg_cursor_t;

static hba_mem_t *g_hba;
static ahci_disk_t g_disk;
static volatile uint32_t g_tag_seq;
static volatile uint32_t g_irq_count;

static uint8_t g_cmd_list[AHCI_MAX_PORTS][1024] __attribute__((aligned(1024)));
static uint8_t g_rx_fis[AHCI_MAX_PORTS][256] __attribute__((aligned(256)));
static hba_cmd_tbl_t g_cmd_tbl[AHCI_MAX_PORTS][AHCI_MAX_CMDS] __attribute__((aligned(128)));
static uint16_t g_identify_buf[256] __attribute__((aligned(2)));

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr = 0x80000000u |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) |
                    (off & 0xFCu);
    outportl(AHCI_PCI_CFG_ADDR, addr);
    return inportl(AHCI_PCI_CFG_DATA);
}

static uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_cfg_read32(bus, slot, func, (uint8_t)(off & 0xFCu));
    return (uint16_t)((v >> ((off & 2u) * 8u)) & 0xFFFFu);
}

static uint8_t pci_cfg_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_cfg_read32(bus, slot, func, (uint8_t)(off & 0xFCu));
    return (uint8_t)((v >> ((off & 3u) * 8u)) & 0xFFu);
}

static void pci_cfg_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t val) {
    uint32_t addr = 0x80000000u |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) |
                    (off & 0xFCu);
    uint32_t cur;
    uint32_t shift = (off & 2u) * 8u;
    outportl(AHCI_PCI_CFG_ADDR, addr);
    cur = inportl(AHCI_PCI_CFG_DATA);
    cur &= ~(0xFFFFu << shift);
    cur |= ((uint32_t)val << shift);
    outportl(AHCI_PCI_CFG_ADDR, addr);
    outportl(AHCI_PCI_CFG_DATA, cur);
}

static void ahci_stop_cmd(hba_port_t *p) {
    uint32_t cmd = p->cmd;
    cmd &= ~AHCI_PxCMD_ST;
    cmd &= ~AHCI_PxCMD_FRE;
    p->cmd = cmd;
    for (int i = 0; i < 1000000; ++i) {
        uint32_t c = p->cmd;
        if ((c & (AHCI_PxCMD_FR | AHCI_PxCMD_CR)) == 0) return;
    }
}

static void ahci_start_cmd(hba_port_t *p) {
    for (int i = 0; i < 1000000; ++i) {
        if ((p->cmd & AHCI_PxCMD_CR) == 0) break;
    }
    p->cmd |= AHCI_PxCMD_FRE;
    p->cmd |= AHCI_PxCMD_ST;
}

static int ahci_wait_ready(hba_port_t *p) {
    for (int i = 0; i < 1000000; ++i) {
        uint32_t tfd = p->tfd;
        if ((tfd & (AHCI_PxTFD_BSY | AHCI_PxTFD_DRQ)) == 0) return 0;
        __asm__ __volatile__("pause");
    }
    return -1;
}

static int ahci_find_slot(hba_port_t *p) {
    uint32_t slots = p->sact | p->ci;
    for (int i = 0; i < AHCI_MAX_CMDS; ++i) {
        if ((slots & (1u << i)) == 0) return i;
    }
    return -1;
}

static int ahci_port_present(hba_port_t *p) {
    uint32_t ssts = p->ssts;
    uint8_t det = (uint8_t)(ssts & 0x0Fu);
    uint8_t ipm = (uint8_t)((ssts >> 8) & 0x0Fu);
    if (det != AHCI_HBA_PORT_DET_PRESENT) return 0;
    if (ipm != AHCI_HBA_PORT_IPM_ACTIVE) return 0;
    return 1;
}

static uint64_t ahci_total_sg_bytes(const ahci_sg_t *sg, uint32_t sg_count) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < sg_count; ++i) total += sg[i].len;
    return total;
}

static int ahci_fill_prdt(hba_cmd_tbl_t *tbl,
                          ahci_sg_cursor_t *cur,
                          uint32_t max_bytes,
                          uint32_t *mapped_bytes,
                          uint16_t *prdtl_out) {
    uint32_t bytes = 0;
    uint16_t prdtl = 0;

    if (!tbl || !cur || !mapped_bytes || !prdtl_out) return -1;
    if ((max_bytes % 512u) != 0) return -1;

    while (prdtl < AHCI_MAX_PRDT && bytes < max_bytes) {
        uint64_t addr;
        uint32_t avail;
        uint32_t take;
        hba_prdt_entry_t *e;

        while (cur->index < cur->sg_count && cur->off >= cur->sg[cur->index].len) {
            cur->index++;
            cur->off = 0;
        }
        if (cur->index >= cur->sg_count) break;

        addr = cur->sg[cur->index].addr + cur->off;
        avail = cur->sg[cur->index].len - cur->off;
        if (avail == 0) {
            cur->index++;
            cur->off = 0;
            continue;
        }

        take = avail;
        if (take > AHCI_PRDT_MAX_BYTES) take = AHCI_PRDT_MAX_BYTES;
        if (take > (max_bytes - bytes)) take = max_bytes - bytes;
        take &= ~511u;
        if (take == 0) break;

        e = &tbl->prdt_entry[prdtl++];
        e->dba = (uint32_t)addr;
        e->dbau = (uint32_t)(addr >> 32);
        e->dbc = take - 1u;
        e->i = 0;

        bytes += take;
        cur->off += take;
    }

    if (bytes == 0 || (bytes % 512u) != 0 || prdtl == 0) return -1;
    tbl->prdt_entry[prdtl - 1].i = 1;
    *mapped_bytes = bytes;
    *prdtl_out = prdtl;
    return 0;
}

static uint32_t ahci_next_tag(void) {
    uint32_t tag = ++g_tag_seq;
    if (tag == 0) tag = ++g_tag_seq;
    return tag;
}

static void ahci_mark_completions(hba_port_t *p, uint32_t port_is) {
    uint32_t active;
    uint32_t finished;

    if (!g_disk.present || !p) return;

    active = p->ci | p->sact;
    finished = g_disk.outstanding_mask & ~active;
    while (finished) {
        uint32_t bit = finished & (~finished + 1u);
        int slot = 0;
        while ((bit >> slot) != 1u) slot++;
        g_disk.slot_error[slot] = 0;
        g_disk.slot_done_tag[slot] = g_disk.slot_tag[slot];
        g_disk.outstanding_mask &= ~bit;
        finished &= ~bit;
    }

    if (port_is & AHCI_PxIS_TFES) {
        uint32_t fail = g_disk.outstanding_mask;
        while (fail) {
            uint32_t bit = fail & (~fail + 1u);
            int slot = 0;
            while ((bit >> slot) != 1u) slot++;
            g_disk.slot_error[slot] = 1;
            g_disk.slot_done_tag[slot] = g_disk.slot_tag[slot];
            fail &= ~bit;
        }
        g_disk.outstanding_mask = 0;
        p->serr = (uint32_t)-1;
    }
}

static void ahci_poll_port_completion(void) {
    hba_port_t *p;
    uint32_t port_is;

    if (!g_disk.present || !g_disk.port) return;
    p = g_disk.port;
    port_is = p->is;
    if (port_is) p->is = port_is;
    ahci_mark_completions(p, port_is);
}

static void ahci_irq_handler(REGISTERS *r) {
    (void)r;
    g_irq_count++;
    ahci_poll_port_completion();
}

static int ahci_wait_slot_done(int slot, uint32_t tag) {
    uint32_t last_irq = g_irq_count;
    uint32_t idle_hlt_spins = 0;
    for (uint32_t i = 0; i < 200000u; ++i) {
        if (g_disk.slot_done_tag[slot] == tag) {
            return g_disk.slot_error[slot] ? -1 : 0;
        }
        ahci_poll_port_completion();
        if (g_disk.slot_done_tag[slot] == tag) {
            return g_disk.slot_error[slot] ? -1 : 0;
        }

        if (!g_disk.irq_enabled) {
            __asm__ __volatile__("pause");
            continue;
        }

        /* Prefer pure polling until we have observed AHCI IRQ activity.
         * Avoid hlt wakeups on timer ticks when AHCI IRQ routing is broken. */
        if (g_irq_count == 0) {
            __asm__ __volatile__("pause");
            continue;
        }

        /* Hybrid wait: mostly polling, occasional interrupt sleep. */
        if ((i & 0x1FFFu) != 0x1FFFu) {
            __asm__ __volatile__("pause");
            continue;
        }

        {
            uint32_t before = g_irq_count;
            __asm__ __volatile__("sti; hlt");
            if (g_irq_count == before && before == last_irq) {
                idle_hlt_spins++;
                if (idle_hlt_spins > 8u) g_disk.irq_enabled = 0;
            } else {
                idle_hlt_spins = 0;
                last_irq = g_irq_count;
            }
        }
    }
    return -1;
}

static void ahci_build_cfis(fis_reg_h2d_t *cfis,
                            int write,
                            uint64_t lba,
                            uint16_t sectors,
                            int slot,
                            int use_ncq) {
    memset(cfis, 0, sizeof(*cfis));
    cfis->fis_type = AHCI_FIS_TYPE_REG_H2D;
    cfis->c = 1;
    cfis->device = 1u << 6;
    cfis->lba0 = (uint8_t)(lba & 0xFFu);
    cfis->lba1 = (uint8_t)((lba >> 8) & 0xFFu);
    cfis->lba2 = (uint8_t)((lba >> 16) & 0xFFu);
    cfis->lba3 = (uint8_t)((lba >> 24) & 0xFFu);
    cfis->lba4 = (uint8_t)((lba >> 32) & 0xFFu);
    cfis->lba5 = (uint8_t)((lba >> 40) & 0xFFu);

    if (use_ncq) {
        cfis->command = write ? AHCI_CMD_WRITE_FPDMA_QUEUED : AHCI_CMD_READ_FPDMA_QUEUED;
        cfis->featurel = (uint8_t)(sectors & 0xFFu);
        cfis->featureh = (uint8_t)((sectors >> 8) & 0xFFu);
        cfis->countl = (uint8_t)(slot << 3);
        cfis->counth = 0;
    } else {
        cfis->command = write ? AHCI_CMD_WRITE_DMA_EXT : AHCI_CMD_READ_DMA_EXT;
        cfis->countl = (uint8_t)(sectors & 0xFFu);
        cfis->counth = (uint8_t)((sectors >> 8) & 0xFFu);
    }
}

static int ahci_submit_rw(hba_port_t *p,
                          int port_no,
                          int slot,
                          int write,
                          uint64_t lba,
                          ahci_sg_cursor_t *cur,
                          uint32_t max_bytes,
                          uint32_t tag,
                          uint32_t *mapped_bytes_out) {
    hba_cmd_header_t *hdr;
    hba_cmd_tbl_t *tbl;
    fis_reg_h2d_t *cfis;
    uint32_t mapped_bytes;
    uint16_t prdtl;
    uint16_t sectors;

    if (!p || !cur || !mapped_bytes_out) return -1;
    if (max_bytes == 0) return -1;

    hdr = ((hba_cmd_header_t *)(void *)g_cmd_list[port_no]) + slot;
    memset(hdr, 0, sizeof(*hdr));
    hdr->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    hdr->w = write ? 1u : 0u;
    hdr->ctba = (uint32_t)(uintptr_t)&g_cmd_tbl[port_no][slot];
    hdr->ctbau = (uint32_t)(((uint64_t)(uintptr_t)&g_cmd_tbl[port_no][slot]) >> 32);

    tbl = &g_cmd_tbl[port_no][slot];
    memset(tbl, 0, sizeof(*tbl));

    if (ahci_fill_prdt(tbl, cur, max_bytes, &mapped_bytes, &prdtl) < 0) return -1;
    sectors = (uint16_t)(mapped_bytes / 512u);
    if (sectors == 0) return -1;

    hdr->prdtl = prdtl;
    cfis = (fis_reg_h2d_t *)(void *)tbl->cfis;
    ahci_build_cfis(cfis, write, lba, sectors, slot, g_disk.ncq_supported);

    g_disk.slot_tag[slot] = tag;
    g_disk.slot_done_tag[slot] = 0;
    g_disk.slot_error[slot] = 0;

    g_disk.outstanding_mask |= (1u << slot);
    p->is = (uint32_t)-1;
    if (g_disk.ncq_supported) p->sact |= (1u << slot);
    p->ci |= (1u << slot);

    *mapped_bytes_out = mapped_bytes;
    return 0;
}

static int ahci_issue_rw_sg(int write, uint32_t lba, uint32_t sector_count, const ahci_sg_t *sg, uint32_t sg_count) {
    hba_port_t *p;
    ahci_sg_cursor_t cur;
    uint64_t remaining_bytes;
    uint64_t cur_lba;
    uint32_t queue_depth;

    if (!g_disk.present || !g_disk.port || !sg || sg_count == 0 || sector_count == 0) return -1;
    if (ahci_total_sg_bytes(sg, sg_count) != (uint64_t)sector_count * 512u) return -1;

    p = g_disk.port;
    queue_depth = g_disk.ncq_supported ? g_disk.queue_depth : 1u;
    if (queue_depth == 0) queue_depth = 1u;
    if (queue_depth > AHCI_MAX_CMDS) queue_depth = AHCI_MAX_CMDS;

    cur.sg = sg;
    cur.sg_count = sg_count;
    cur.index = 0;
    cur.off = 0;
    remaining_bytes = (uint64_t)sector_count * 512u;
    cur_lba = lba;

    while (remaining_bytes > 0) {
        int pending_slots[AHCI_MAX_CMDS];
        uint32_t pending_tags[AHCI_MAX_CMDS];
        uint32_t pending_count = 0;

        while (pending_count < queue_depth && remaining_bytes > 0) {
            int slot;
            uint32_t tag;
            uint32_t max_bytes;
            uint32_t mapped_bytes;

            if (!g_disk.ncq_supported && ahci_wait_ready(p) < 0) return -1;

            slot = ahci_find_slot(p);
            if (slot < 0) break;

            tag = ahci_next_tag();
            max_bytes = (uint32_t)remaining_bytes;
            if (max_bytes > AHCI_MAX_CMD_SECTORS * 512u) max_bytes = AHCI_MAX_CMD_SECTORS * 512u;
            if (max_bytes > AHCI_MAX_PRDT * AHCI_PRDT_MAX_BYTES) max_bytes = AHCI_MAX_PRDT * AHCI_PRDT_MAX_BYTES;
            max_bytes &= ~511u;
            if (max_bytes == 0) return -1;

            if (ahci_submit_rw(p,
                               g_disk.port_no,
                               slot,
                               write,
                               cur_lba,
                               &cur,
                               max_bytes,
                               tag,
                               &mapped_bytes) < 0) {
                return -1;
            }

            pending_slots[pending_count] = slot;
            pending_tags[pending_count] = tag;
            pending_count++;

            remaining_bytes -= mapped_bytes;
            cur_lba += (uint64_t)(mapped_bytes / 512u);
        }

        if (pending_count == 0) {
            ahci_poll_port_completion();
            if (g_disk.irq_enabled) __asm__ __volatile__("sti; hlt");
            else __asm__ __volatile__("pause");
            continue;
        }

        for (uint32_t i = 0; i < pending_count; ++i) {
            if (ahci_wait_slot_done(pending_slots[i], pending_tags[i]) < 0) return -1;
        }
    }

    return 0;
}

static int ahci_identify(hba_port_t *p, int port_no, uint16_t *ident_out) {
    int slot;
    hba_cmd_header_t *hdr;
    hba_cmd_tbl_t *tbl;
    fis_reg_h2d_t *cfis;
    uint64_t phys = (uint64_t)(uintptr_t)ident_out;

    if (!p || !ident_out) return -1;
    if (ahci_wait_ready(p) < 0) return -1;
    slot = ahci_find_slot(p);
    if (slot < 0) return -1;

    hdr = ((hba_cmd_header_t *)(void *)g_cmd_list[port_no]) + slot;
    memset(hdr, 0, sizeof(*hdr));
    hdr->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    hdr->w = 0;
    hdr->prdtl = 1;
    hdr->ctba = (uint32_t)(uintptr_t)&g_cmd_tbl[port_no][slot];
    hdr->ctbau = (uint32_t)(((uint64_t)(uintptr_t)&g_cmd_tbl[port_no][slot]) >> 32);

    tbl = &g_cmd_tbl[port_no][slot];
    memset(tbl, 0, sizeof(*tbl));
    tbl->prdt_entry[0].dba = (uint32_t)phys;
    tbl->prdt_entry[0].dbau = (uint32_t)(phys >> 32);
    tbl->prdt_entry[0].dbc = 512u - 1u;
    tbl->prdt_entry[0].i = 1;

    cfis = (fis_reg_h2d_t *)(void *)tbl->cfis;
    memset(cfis, 0, sizeof(*cfis));
    cfis->fis_type = AHCI_FIS_TYPE_REG_H2D;
    cfis->c = 1;
    cfis->command = AHCI_CMD_IDENTIFY_DEVICE;
    cfis->countl = 1;

    p->is = (uint32_t)-1;
    p->ci |= (1u << slot);

    for (uint32_t i = 0; i < 200000u; ++i) {
        if ((p->ci & (1u << slot)) == 0) {
            if (p->is & AHCI_PxIS_TFES) return -1;
            return 0;
        }
        if (p->is & AHCI_PxIS_TFES) return -1;
        __asm__ __volatile__("pause");
    }
    return -1;
}

int ahci_present(void) {
    return g_disk.present;
}

uint32_t ahci_sector_count(void) {
    return g_disk.present ? g_disk.sectors : 0;
}

int ahci_read_sg(uint32_t lba, uint32_t sector_count, const ahci_sg_t *sg, uint32_t sg_count) {
    return ahci_issue_rw_sg(0, lba, sector_count, sg, sg_count);
}

int ahci_write_sg(uint32_t lba, uint32_t sector_count, const ahci_sg_t *sg, uint32_t sg_count) {
    return ahci_issue_rw_sg(1, lba, sector_count, sg, sg_count);
}

int ahci_read(uint32_t lba, uint32_t sector_count, void *buf) {
    ahci_sg_t sg;
    if (!buf || sector_count == 0) return -1;
    sg.addr = (uint64_t)(uintptr_t)buf;
    sg.len = sector_count * 512u;
    return ahci_read_sg(lba, sector_count, &sg, 1);
}

int ahci_write(uint32_t lba, uint32_t sector_count, const void *buf) {
    ahci_sg_t sg;
    if (!buf || sector_count == 0) return -1;
    sg.addr = (uint64_t)(uintptr_t)buf;
    sg.len = sector_count * 512u;
    return ahci_write_sg(lba, sector_count, &sg, 1);
}

int ahci_init(void) {
    uint8_t found_bus = 0, found_dev = 0, found_fn = 0;
    int found = 0;
    uint64_t abar;
    uint32_t pi;

    memset(&g_disk, 0, sizeof(g_disk));
    g_hba = 0;

    bootlog_stage("Probing AHCI controller");

    for (uint16_t bus = 0; bus < 256 && !found; ++bus) {
        for (uint8_t dev = 0; dev < 32 && !found; ++dev) {
            for (uint8_t fn = 0; fn < 8; ++fn) {
                uint16_t ven = pci_cfg_read16((uint8_t)bus, dev, fn, 0x00);
                if (ven == 0xFFFFu) continue;
                if (pci_cfg_read8((uint8_t)bus, dev, fn, 0x0B) != AHCI_PCI_CLASS_MASS_STORAGE) continue;
                if (pci_cfg_read8((uint8_t)bus, dev, fn, 0x0A) != AHCI_PCI_SUBCLASS_SATA) continue;
                if (pci_cfg_read8((uint8_t)bus, dev, fn, 0x09) != AHCI_PCI_PROGIF_AHCI) continue;
                found = 1;
                found_bus = (uint8_t)bus;
                found_dev = dev;
                found_fn = fn;
                break;
            }
        }
    }

    if (!found) return -1;

    {
        uint16_t cmd = pci_cfg_read16(found_bus, found_dev, found_fn, 0x04);
        cmd |= 0x0006u;
        cmd &= (uint16_t)~0x0400u;
        pci_cfg_write16(found_bus, found_dev, found_fn, 0x04, cmd);
    }

    abar = (uint64_t)(pci_cfg_read32(found_bus, found_dev, found_fn, 0x24) & ~0x0Fu);
    if (!abar) return -1;
    g_hba = (hba_mem_t *)(uintptr_t)abar;
    g_hba->ghc |= AHCI_HBA_GHC_AE;

    pi = g_hba->pi;
    for (int port = 0; port < AHCI_MAX_PORTS; ++port) {
        hba_port_t *p;
        uint64_t clb;
        uint64_t fb;
        uint64_t lba48;

        if ((pi & (1u << port)) == 0) continue;
        p = &g_hba->ports[port];
        if (!ahci_port_present(p)) continue;
        if (p->sig != AHCI_SIG_ATA) continue;

        ahci_stop_cmd(p);
        memset(g_cmd_list[port], 0, sizeof(g_cmd_list[port]));
        memset(g_rx_fis[port], 0, sizeof(g_rx_fis[port]));
        memset(g_cmd_tbl[port], 0, sizeof(g_cmd_tbl[port]));

        clb = (uint64_t)(uintptr_t)g_cmd_list[port];
        fb = (uint64_t)(uintptr_t)g_rx_fis[port];
        p->clb = (uint32_t)clb;
        p->clbu = (uint32_t)(clb >> 32);
        p->fb = (uint32_t)fb;
        p->fbu = (uint32_t)(fb >> 32);

        for (int i = 0; i < AHCI_MAX_CMDS; ++i) {
            hba_cmd_header_t *hdr = ((hba_cmd_header_t *)(void *)g_cmd_list[port]) + i;
            uint64_t ctba = (uint64_t)(uintptr_t)&g_cmd_tbl[port][i];
            hdr->prdtl = AHCI_MAX_PRDT;
            hdr->ctba = (uint32_t)ctba;
            hdr->ctbau = (uint32_t)(ctba >> 32);
        }

        p->serr = (uint32_t)-1;
        p->is = (uint32_t)-1;
        p->ie = (uint32_t)-1;
        ahci_start_cmd(p);

        memset(g_identify_buf, 0, sizeof(g_identify_buf));
        if (ahci_identify(p, port, g_identify_buf) < 0) continue;

        g_disk.present = 1;
        g_disk.port_no = port;
        g_disk.port = p;
        g_disk.sectors = (uint32_t)g_identify_buf[60] | ((uint32_t)g_identify_buf[61] << 16);

        lba48 = (uint64_t)g_identify_buf[100] |
                ((uint64_t)g_identify_buf[101] << 16) |
                ((uint64_t)g_identify_buf[102] << 32) |
                ((uint64_t)g_identify_buf[103] << 48);
        if (lba48 != 0) {
            if (lba48 < 0xFFFFFFFFu) g_disk.sectors = (uint32_t)lba48;
            else g_disk.sectors = 0xFFFFFFFFu;
        }

        g_disk.ncq_supported = ((g_identify_buf[76] & (1u << 8)) != 0) ? 1 : 0;
        /* QEMU/ICH9 can advertise NCQ but still behave poorly with FPDMA queued
         * under this minimal driver. Keep DMA EXT path for boot reliability. */
        g_disk.ncq_supported = 0;
        g_disk.queue_depth = (uint32_t)((g_identify_buf[75] & 0x1Fu) + 1u);
        if (!g_disk.ncq_supported) g_disk.queue_depth = 1;
        if (g_disk.queue_depth == 0) g_disk.queue_depth = 1;
        if (g_disk.queue_depth > AHCI_MAX_CMDS) g_disk.queue_depth = AHCI_MAX_CMDS;

        g_disk.irq_line = pci_cfg_read8(found_bus, found_dev, found_fn, 0x3C);
        g_disk.irq_enabled = 0;
        g_irq_count = 0;
        if (g_disk.irq_line < 16u && !isr_interrupt_has_handler(IRQ_BASE + g_disk.irq_line)) {
            isr_register_interrupt_handler(IRQ_BASE + g_disk.irq_line, ahci_irq_handler);
            pic8259_unmask_irq(g_disk.irq_line);
            g_disk.irq_enabled = 1;
            g_hba->ghc |= AHCI_HBA_GHC_IE;
        } else {
            g_hba->ghc &= ~AHCI_HBA_GHC_IE;
        }

        bootlog_stage("AHCI SATA disk ready");
        printf("[ahci] controller %u:%u.%u port=%d sectors=%u qd=%u ncq=%d irq=%u mode=%s\n",
               (uint32_t)found_bus, (uint32_t)found_dev, (uint32_t)found_fn,
               port, g_disk.sectors, g_disk.queue_depth, g_disk.ncq_supported,
               (uint32_t)g_disk.irq_line,
               g_disk.irq_enabled ? "interrupt" : "poll");
        return 0;
    }

    return -1;
}
