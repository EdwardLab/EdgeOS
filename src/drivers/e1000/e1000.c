#include "drivers/e1000.h"

#include "8259_pic.h"
#include "isr.h"
#include "io_ports.h"
#include "stdio.h"
#include "string.h"
#include "sys/boottime.h"

#include <stdint.h>

#define E1000_VENDOR_INTEL 0x8086u
#define PCI_CFG_ADDR_PORT 0xCF8u
#define PCI_CFG_DATA_PORT 0xCFCu

#define E1000_REG_CTRL    0x0000
#define E1000_REG_STATUS  0x0008
#define E1000_REG_EECD    0x0010
#define E1000_REG_ICR     0x00C0
#define E1000_REG_IMS     0x00D0
#define E1000_REG_IMC     0x00D8
#define E1000_REG_RCTL    0x0100
#define E1000_REG_TCTL    0x0400
#define E1000_REG_TIPG    0x0410
#define E1000_REG_RDBAL   0x2800
#define E1000_REG_RDBAH   0x2804
#define E1000_REG_RDLEN   0x2808
#define E1000_REG_RDH     0x2810
#define E1000_REG_RDT     0x2818
#define E1000_REG_TDBAL   0x3800
#define E1000_REG_TDBAH   0x3804
#define E1000_REG_TDLEN   0x3808
#define E1000_REG_TDH     0x3810
#define E1000_REG_TDT     0x3818
#define E1000_REG_RAL     0x5400
#define E1000_REG_RAH     0x5404

#define E1000_CTRL_RST        (1u << 26)
#define E1000_CTRL_ASDE       (1u << 5)
#define E1000_CTRL_SLU        (1u << 6)
#define E1000_RCTL_EN         (1u << 1)
#define E1000_RCTL_UPE        (1u << 3)
#define E1000_RCTL_MPE        (1u << 4)
#define E1000_RCTL_BAM        (1u << 15)
#define E1000_RCTL_SECRC      (1u << 26)
#define E1000_TCTL_EN         (1u << 1)
#define E1000_TCTL_PSP        (1u << 3)
#define E1000_RAH_AV          (1u << 31)
#define E1000_TX_CMD_EOP      (1u << 0)
#define E1000_TX_CMD_IFCS     (1u << 1)
#define E1000_TX_CMD_RS       (1u << 3)
#define E1000_TX_STATUS_DD    (1u << 0)
#define E1000_RX_STATUS_DD    (1u << 0)
#define E1000_RX_STATUS_EOP   (1u << 1)

#define E1000_ICR_TXDW        (1u << 0)
#define E1000_ICR_LSC         (1u << 2)
#define E1000_ICR_RXDMT0      (1u << 4)
#define E1000_ICR_RXO         (1u << 6)
#define E1000_ICR_RXT0        (1u << 7)
#define E1000_IMS_RX_MASK     (E1000_ICR_RXDMT0 | E1000_ICR_RXO | E1000_ICR_RXT0)

#define E1000_RX_DESC_COUNT 64
#define E1000_TX_DESC_COUNT 16
#define E1000_RX_BUF_SIZE 2048
#define E1000_TX_BUF_SIZE 2048
#define E1000_RX_QUEUE_MAX 16

#define ETH_TYPE_ARP 0x0806
#define ETH_TYPE_IP4 0x0800

typedef struct __attribute__((packed)) {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t ethertype_be;
} eth_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t htype_be;
    uint16_t ptype_be;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper_be;
    uint8_t sha[6];
    uint32_t spa_be;
    uint8_t tha[6];
    uint32_t tpa_be;
} arp_pkt_t;

typedef struct __attribute__((packed)) {
    uint8_t ver_ihl;
    uint8_t tos;
    uint16_t total_len_be;
    uint16_t id_be;
    uint16_t frag_be;
    uint8_t ttl;
    uint8_t proto;
    uint16_t csum_be;
    uint32_t src_be;
    uint32_t dst_be;
} ipv4_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t code;
    uint16_t csum_be;
    uint16_t id_be;
    uint16_t seq_be;
} icmp_echo_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} e1000_rx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} e1000_tx_desc_t;

typedef struct {
    uint32_t len;
    uint32_t src_ip_be;
    uint8_t data[1600];
} rx_ip_pkt_t;

static volatile uint8_t *g_mmio;
static int g_ready;
static uint8_t g_mac[6];
static e1000_rx_frame_cb_t g_rx_frame_cb;
static uint32_t g_host_ip_be = 0x0F02000Au;   /* 10.0.2.15 in BE */
static uint32_t g_gateway_ip_be = 0x0202000Au;/* 10.0.2.2 in BE */
static int g_arp_valid;
static uint32_t g_arp_ip_be;
static uint8_t g_arp_mac[6];

static e1000_rx_desc_t g_rx_desc[E1000_RX_DESC_COUNT] __attribute__((aligned(16)));
static e1000_tx_desc_t g_tx_desc[E1000_TX_DESC_COUNT] __attribute__((aligned(16)));
static uint8_t g_rx_buf[E1000_RX_DESC_COUNT][E1000_RX_BUF_SIZE] __attribute__((aligned(16)));
static uint8_t g_tx_buf[E1000_TX_DESC_COUNT][E1000_TX_BUF_SIZE] __attribute__((aligned(16)));
static uint32_t g_rx_cur;
static uint32_t g_tx_cur;
static uint32_t g_dbg_tx_sent;
static uint32_t g_dbg_tx_timeout;
static uint32_t g_dbg_rx_seen;
static uint8_t g_irq_line = 0xFFu;
static uint32_t g_irq_count;
static uint32_t g_poll_count;

static rx_ip_pkt_t g_rx_ip_queue[E1000_RX_QUEUE_MAX];
static uint32_t g_rx_ip_count;

static uint16_t bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static uint32_t bswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) |
           ((v & 0xFF000000u) >> 24);
}

static uint16_t csum16(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t acc = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2) acc += ((uint32_t)p[i] << 8) | p[i + 1];
    if (len & 1u) acc += (uint32_t)p[len - 1] << 8;
    while (acc >> 16) acc = (acc & 0xFFFFu) + (acc >> 16);
    return (uint16_t)~acc;
}

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr = 0x80000000u |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) |
                    (off & 0xFCu);
    outportl(PCI_CFG_ADDR_PORT, addr);
    return inportl(PCI_CFG_DATA_PORT);
}

static void pci_cfg_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v) {
    uint32_t addr = 0x80000000u |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) |
                    (off & 0xFCu);
    outportl(PCI_CFG_ADDR_PORT, addr);
    outportl(PCI_CFG_DATA_PORT, v);
}

static uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_cfg_read32(bus, slot, func, (uint8_t)(off & 0xFCu));
    uint8_t sh = (uint8_t)((off & 2u) * 8u);
    return (uint16_t)((v >> sh) & 0xFFFFu);
}

static uint8_t pci_cfg_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_cfg_read32(bus, slot, func, (uint8_t)(off & 0xFCu));
    uint8_t sh = (uint8_t)((off & 3u) * 8u);
    return (uint8_t)((v >> sh) & 0xFFu);
}

static uint32_t e1000_rd(uint32_t reg) { return *(volatile uint32_t *)(g_mmio + reg); }
static void e1000_wr(uint32_t reg, uint32_t val) { *(volatile uint32_t *)(g_mmio + reg) = val; }

static int e1000_send_frame(const void *frame, uint16_t len) {
    e1000_tx_desc_t *d;
    e1000_tx_desc_t *sent;
    uint32_t next;
    uint16_t wire_len;
    if (!g_ready || len == 0 || len > E1000_TX_BUF_SIZE) return -1;
    d = &g_tx_desc[g_tx_cur];
    if ((d->status & E1000_TX_STATUS_DD) == 0) return -1;
    wire_len = len < 60 ? 60 : len;
    memcpy(g_tx_buf[g_tx_cur], frame, len);
    if (wire_len > len) memset(g_tx_buf[g_tx_cur] + len, 0, wire_len - len);
    d->length = wire_len;
    d->cmd = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    d->status = 0;
    next = (g_tx_cur + 1u) % E1000_TX_DESC_COUNT;
    e1000_wr(E1000_REG_TDT, next);
    sent = d;
    g_tx_cur = next;
    for (volatile uint32_t t = 0; t < 200000u; ++t) {
        if (sent->status & E1000_TX_STATUS_DD) break;
    }
    if ((sent->status & E1000_TX_STATUS_DD) == 0) {
        if (g_dbg_tx_timeout < 4) {
            printf("[net] e1000: tx timeout tdt=%u tdh=%u status=0x%x\n",
                   (uint32_t)e1000_rd(E1000_REG_TDT),
                   (uint32_t)e1000_rd(E1000_REG_TDH),
                   (uint32_t)e1000_rd(E1000_REG_STATUS));
        }
        g_dbg_tx_timeout++;
        return -1;
    }
    (void)wire_len;
    g_dbg_tx_sent++;
    return 0;
}

static void queue_ipv4_packet(const uint8_t *pkt, uint32_t len, uint32_t src_be) {
    if (!pkt || len == 0 || len > sizeof(g_rx_ip_queue[0].data)) return;
    if (g_rx_ip_count >= E1000_RX_QUEUE_MAX) return;
    g_rx_ip_queue[g_rx_ip_count].len = len;
    g_rx_ip_queue[g_rx_ip_count].src_ip_be = src_be;
    memcpy(g_rx_ip_queue[g_rx_ip_count].data, pkt, len);
    g_rx_ip_count++;
}

static void e1000_handle_rx_frame(const uint8_t *frm, uint32_t len) {
    const eth_hdr_t *eh;
    if (len < sizeof(eth_hdr_t)) return;
    eh = (const eth_hdr_t *)frm;
    g_dbg_rx_seen++;
    if (g_rx_frame_cb) g_rx_frame_cb(frm, len);
    if (bswap16(eh->ethertype_be) == ETH_TYPE_ARP) {
        const arp_pkt_t *arp;
        if (len < sizeof(eth_hdr_t) + sizeof(arp_pkt_t)) return;
        arp = (const arp_pkt_t *)(frm + sizeof(eth_hdr_t));
        if (bswap16(arp->oper_be) == 2 && arp->spa_be == g_gateway_ip_be) {
            g_arp_valid = 1;
            g_arp_ip_be = arp->spa_be;
            memcpy(g_arp_mac, arp->sha, 6);
        }
        return;
    }
    if (bswap16(eh->ethertype_be) == ETH_TYPE_IP4) {
        const ipv4_hdr_t *ip;
        uint32_t iplen;
        if (len < sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t)) return;
        ip = (const ipv4_hdr_t *)(frm + sizeof(eth_hdr_t));
        if ((ip->ver_ihl >> 4) != 4) return;
        iplen = (uint32_t)bswap16(ip->total_len_be);
        if (iplen < sizeof(ipv4_hdr_t)) return;
        if (sizeof(eth_hdr_t) + iplen > len) return;
        if (ip->dst_be != g_host_ip_be) return;
        if (ip->proto != 1) return;
        queue_ipv4_packet((const uint8_t *)ip, iplen, ip->src_be);
    }
}

int e1000_send_frame_raw(const void *frame, uint16_t len) {
    return e1000_send_frame(frame, len);
}

void e1000_set_rx_frame_callback(e1000_rx_frame_cb_t cb) {
    g_rx_frame_cb = cb;
}

static void e1000_poll_rx(const char *src) {
    while (g_ready) {
        e1000_rx_desc_t *d = &g_rx_desc[g_rx_cur];
        if ((d->status & E1000_RX_STATUS_DD) == 0) break;
        (void)src;
        if (d->length > 0 && d->length <= E1000_RX_BUF_SIZE) {
            e1000_handle_rx_frame(g_rx_buf[g_rx_cur], d->length);
        }
        d->status = 0;
        e1000_wr(E1000_REG_RDT, g_rx_cur);
        g_rx_cur = (g_rx_cur + 1u) % E1000_RX_DESC_COUNT;
    }
}

static void e1000_irq_handler(REGISTERS *reg) {
    uint32_t icr;
    (void)reg;
    if (!g_ready) return;
    g_irq_count++;
    icr = e1000_rd(E1000_REG_ICR); /* acknowledge causes by reading ICR */
    if (!icr) return;
    if (icr & E1000_IMS_RX_MASK) {
        e1000_poll_rx("irq");
    }
}

static void e1000_send_arp_request(uint32_t target_ip_be) {
    uint8_t frame[64];
    eth_hdr_t *eh = (eth_hdr_t *)frame;
    arp_pkt_t *arp = (arp_pkt_t *)(frame + sizeof(eth_hdr_t));
    memset(frame, 0, sizeof(frame));
    memset(eh->dst, 0xFF, 6);
    memcpy(eh->src, g_mac, 6);
    eh->ethertype_be = bswap16(ETH_TYPE_ARP);
    arp->htype_be = bswap16(1);
    arp->ptype_be = bswap16(ETH_TYPE_IP4);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper_be = bswap16(1);
    memcpy(arp->sha, g_mac, 6);
    arp->spa_be = g_host_ip_be;
    memset(arp->tha, 0, 6);
    arp->tpa_be = target_ip_be;
    (void)e1000_send_frame(frame, sizeof(frame));
}

static int e1000_resolve_gateway_mac(void) {
    if (g_arp_valid && g_arp_ip_be == g_gateway_ip_be) return 0;
    e1000_send_arp_request(g_gateway_ip_be);
    {
        uint64_t until = boottime_monotonic_us() + 500000ull;
        while (boottime_monotonic_us() < until) {
            g_poll_count++;
            e1000_poll_rx("poll");
            if (g_arp_valid && g_arp_ip_be == g_gateway_ip_be) return 0;
        }
    }
    return -1;
}

void e1000_init(void) {
    uint8_t bus = 0, dev = 0, fn = 0;
    uint8_t found_bus = 0, found_dev = 0, found_fn = 0;
    uint16_t ven = 0xFFFF, did = 0xFFFF;
    uint64_t mmio_base = 0;
    uint16_t cmd;
    uint8_t irq_line;
    int found = 0;

    g_ready = 0;
    for (bus = 0; bus < 255 && !found; ++bus) {
        for (dev = 0; dev < 32 && !found; ++dev) {
            for (fn = 0; fn < 8; ++fn) {
                ven = pci_cfg_read16(bus, dev, fn, 0x00);
                if (ven == 0xFFFFu) {
                    if (fn == 0) break;
                    continue;
                }
                did = pci_cfg_read16(bus, dev, fn, 0x02);
                if (ven == E1000_VENDOR_INTEL &&
                    (did == 0x100Eu || did == 0x100Fu || did == 0x10D3u || did == 0x153Au)) {
                    found_bus = bus;
                    found_dev = dev;
                    found_fn = fn;
                    found = 1;
                    break;
                }
            }
        }
    }

    if (!found) {
        printf("[net] e1000: not found\n");
        return;
    }

    for (uint8_t off = 0x10; off <= 0x24; off += 4) {
        uint32_t bar = pci_cfg_read32(found_bus, found_dev, found_fn, off);
        if ((bar & 1u) != 0u) continue; /* I/O BAR */
        if ((bar & ~0xFu) == 0u) continue;
        if (((bar >> 1) & 0x3u) == 0x2u && off <= 0x20) {
            uint32_t bar_hi = pci_cfg_read32(found_bus, found_dev, found_fn, (uint8_t)(off + 4));
            mmio_base = ((uint64_t)bar_hi << 32) | (uint64_t)(bar & ~0xFu);
            break;
        }
        mmio_base = (uint64_t)(bar & ~0xFu);
        break;
    }
    if (mmio_base == 0) {
        printf("[net] e1000: no MMIO BAR found\n");
        return;
    }
    g_mmio = (volatile uint8_t *)(uintptr_t)mmio_base;
    cmd = pci_cfg_read16(found_bus, found_dev, found_fn, 0x04);
    cmd |= 0x0006u;
    pci_cfg_write32(found_bus, found_dev, found_fn, 0x04,
                    (pci_cfg_read32(found_bus, found_dev, found_fn, 0x04) & 0xFFFF0000u) | cmd);

    irq_line = pci_cfg_read8(found_bus, found_dev, found_fn, 0x3C);
    g_irq_line = irq_line;

    e1000_wr(E1000_REG_IMC, 0xFFFFFFFFu);
    e1000_rd(E1000_REG_ICR);
    e1000_wr(E1000_REG_CTRL, e1000_rd(E1000_REG_CTRL) | E1000_CTRL_RST);
    for (volatile uint32_t i = 0; i < 100000; ++i) {}
    e1000_wr(E1000_REG_CTRL, e1000_rd(E1000_REG_CTRL) | E1000_CTRL_SLU | E1000_CTRL_ASDE);

    {
        uint32_t ral = e1000_rd(E1000_REG_RAL);
        uint32_t rah = e1000_rd(E1000_REG_RAH);
        g_mac[0] = (uint8_t)(ral & 0xFFu);
        g_mac[1] = (uint8_t)((ral >> 8) & 0xFFu);
        g_mac[2] = (uint8_t)((ral >> 16) & 0xFFu);
        g_mac[3] = (uint8_t)((ral >> 24) & 0xFFu);
        g_mac[4] = (uint8_t)(rah & 0xFFu);
        g_mac[5] = (uint8_t)((rah >> 8) & 0xFFu);
    }
    {
        uint32_t ral = (uint32_t)g_mac[0] |
                       ((uint32_t)g_mac[1] << 8) |
                       ((uint32_t)g_mac[2] << 16) |
                       ((uint32_t)g_mac[3] << 24);
        uint32_t rah = (uint32_t)g_mac[4] |
                       ((uint32_t)g_mac[5] << 8) |
                       E1000_RAH_AV;
        e1000_wr(E1000_REG_RAL, ral);
        e1000_wr(E1000_REG_RAH, rah);
    }

    memset(g_rx_desc, 0, sizeof(g_rx_desc));
    memset(g_tx_desc, 0, sizeof(g_tx_desc));
    for (uint32_t i = 0; i < E1000_RX_DESC_COUNT; ++i) g_rx_desc[i].addr = (uint64_t)(uintptr_t)&g_rx_buf[i][0];
    for (uint32_t i = 0; i < E1000_TX_DESC_COUNT; ++i) {
        g_tx_desc[i].addr = (uint64_t)(uintptr_t)&g_tx_buf[i][0];
        g_tx_desc[i].status = E1000_TX_STATUS_DD;
    }
    g_rx_cur = 0;
    g_tx_cur = 0;
    g_rx_ip_count = 0;
    g_arp_valid = 0;

    e1000_wr(E1000_REG_RDBAL, (uint32_t)(uintptr_t)&g_rx_desc[0]);
    e1000_wr(E1000_REG_RDBAH, 0);
    e1000_wr(E1000_REG_RDLEN, sizeof(g_rx_desc));
    e1000_wr(E1000_REG_RDH, 0);
    e1000_wr(E1000_REG_RDT, E1000_RX_DESC_COUNT - 1);

    e1000_wr(E1000_REG_TDBAL, (uint32_t)(uintptr_t)&g_tx_desc[0]);
    e1000_wr(E1000_REG_TDBAH, 0);
    e1000_wr(E1000_REG_TDLEN, sizeof(g_tx_desc));
    e1000_wr(E1000_REG_TDH, 0);
    e1000_wr(E1000_REG_TDT, 0);

    e1000_wr(E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_UPE | E1000_RCTL_MPE | E1000_RCTL_BAM | E1000_RCTL_SECRC);
    e1000_wr(E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | (0x10u << 4) | (0x40u << 12));
    e1000_wr(E1000_REG_TIPG, 0x0060200Au);
    e1000_rd(E1000_REG_ICR); /* clear any pending causes before unmask */
    e1000_wr(E1000_REG_IMS, E1000_IMS_RX_MASK);

    if (irq_line < 16u && !isr_interrupt_has_handler(IRQ_BASE + irq_line)) {
        isr_register_interrupt_handler(IRQ_BASE + irq_line, e1000_irq_handler);
        pic8259_unmask_irq(irq_line);
        printf("[net] e1000: irq line %u unmasked and handler installed\n", (uint32_t)irq_line);
    } else if (irq_line < 16u) {
        e1000_wr(E1000_REG_IMC, 0xFFFFFFFFu);
        e1000_rd(E1000_REG_ICR);
        printf("[net] e1000: irq line %u already in use, using polling mode\n", (uint32_t)irq_line);
    } else {
        e1000_wr(E1000_REG_IMC, 0xFFFFFFFFu);
        e1000_rd(E1000_REG_ICR);
        printf("[net] e1000: pci irq line invalid (%u), IRQ RX path disabled\n", (uint32_t)irq_line);
    }

    g_ready = 1;
    printf("[net] e1000: ready bus=%u dev=%u fn=%u mac=%x:%x:%x:%x:%x:%x irq=%u status=0x%x ims=0x%x rdh=%u rdt=%u\n",
           (uint32_t)found_bus, (uint32_t)found_dev, (uint32_t)found_fn,
           g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5],
           (uint32_t)irq_line,
           e1000_rd(E1000_REG_STATUS),
           e1000_rd(E1000_REG_IMS),
           e1000_rd(E1000_REG_RDH),
           e1000_rd(E1000_REG_RDT));
}

int e1000_is_ready(void) {
    return g_ready ? 1 : 0;
}

int e1000_get_mac(uint8_t mac_out[6]) {
    if (!g_ready || !mac_out) return -1;
    memcpy(mac_out, g_mac, 6);
    return 0;
}

void e1000_poll(void) {
    g_poll_count++;
    e1000_poll_rx("poll");
}

int e1000_send_icmp_echo(uint32_t dst_ip_be, const uint8_t *icmp_payload, uint16_t icmp_len) {
    uint8_t frm[1514];
    eth_hdr_t *eh;
    ipv4_hdr_t *ip;
    uint16_t ip_len;
    uint16_t frm_len;
    uint8_t *icmp;
    if (!g_ready || !icmp_payload || icmp_len < sizeof(icmp_echo_t)) return -1;
    if (icmp_len > 1400) return -1;
    if (e1000_resolve_gateway_mac() < 0) return -1;

    eh = (eth_hdr_t *)frm;
    memcpy(eh->dst, g_arp_mac, 6);
    memcpy(eh->src, g_mac, 6);
    eh->ethertype_be = bswap16(ETH_TYPE_IP4);

    ip = (ipv4_hdr_t *)(frm + sizeof(eth_hdr_t));
    memset(ip, 0, sizeof(*ip));
    ip->ver_ihl = 0x45;
    ip->ttl = 64;
    ip->proto = 1;
    ip_len = (uint16_t)(sizeof(ipv4_hdr_t) + icmp_len);
    ip->total_len_be = bswap16(ip_len);
    ip->src_be = g_host_ip_be;
    ip->dst_be = dst_ip_be;
    ip->csum_be = csum16(ip, sizeof(ipv4_hdr_t));

    icmp = (uint8_t *)(ip + 1);
    memcpy(icmp, icmp_payload, icmp_len);
    ((icmp_echo_t *)icmp)->csum_be = 0;
    ((icmp_echo_t *)icmp)->csum_be = csum16(icmp, icmp_len);

    frm_len = (uint16_t)(sizeof(eth_hdr_t) + ip_len);
    if (frm_len < 60) {
        memset(frm + frm_len, 0, 60 - frm_len);
        frm_len = 60;
    }
    return e1000_send_frame(frm, frm_len);
}

int e1000_recv_icmp_reply_for_id(uint16_t id_be, uint8_t *ip_packet_out, uint32_t *ip_packet_len, uint32_t *src_ip_be) {
    g_poll_count++;
    e1000_poll_rx("poll");
    for (uint32_t i = 0; i < g_rx_ip_count; ++i) {
        const ipv4_hdr_t *ip = (const ipv4_hdr_t *)g_rx_ip_queue[i].data;
        uint32_t ihl = (uint32_t)(ip->ver_ihl & 0x0Fu) * 4u;
        if (g_rx_ip_queue[i].len < ihl + sizeof(icmp_echo_t)) continue;
        if (ip->proto != 1) continue;
        const icmp_echo_t *ic = (const icmp_echo_t *)(g_rx_ip_queue[i].data + ihl);
        if (ic->type != 0) continue;
        if (ic->id_be != id_be) continue;
        if (ip_packet_out && ip_packet_len && *ip_packet_len >= g_rx_ip_queue[i].len) {
            memcpy(ip_packet_out, g_rx_ip_queue[i].data, g_rx_ip_queue[i].len);
            *ip_packet_len = g_rx_ip_queue[i].len;
            if (src_ip_be) *src_ip_be = g_rx_ip_queue[i].src_ip_be;
        } else if (ip_packet_len) {
            *ip_packet_len = g_rx_ip_queue[i].len;
            return -1;
        }

        for (uint32_t j = i + 1; j < g_rx_ip_count; ++j) g_rx_ip_queue[j - 1] = g_rx_ip_queue[j];
        g_rx_ip_count--;
        return 1;
    }
    return 0;
}
