#include "drivers/xhci.h"
#include "drivers/usb.h"
#include "io_ports.h"
#include "stdio.h"
#include "string.h"

#define PCI_CFG_ADDR_PORT 0xCF8u
#define PCI_CFG_DATA_PORT 0xCFCu

#define XHCI_USBCMD 0x00u
#define XHCI_USBSTS 0x04u
#define XHCI_PAGESIZE 0x08u
#define XHCI_DNCTRL 0x14u
#define XHCI_CRCR 0x18u
#define XHCI_DCBAAP 0x30u
#define XHCI_CONFIG 0x38u
#define XHCI_PORTSC_BASE 0x400u
#define XHCI_PORTSC_STRIDE 0x10u

#define XHCI_CMD_RUN   (1u << 0)
#define XHCI_CMD_HCRST (1u << 1)
#define XHCI_CMD_INTE  (1u << 2)
#define XHCI_STS_HCH   (1u << 0)
#define XHCI_STS_EINT  (1u << 3)
#define XHCI_STS_CNR   (1u << 11)

#define XHCI_PORTSC_CCS          (1u << 0)
#define XHCI_PORTSC_PR           (1u << 4)
#define XHCI_PORTSC_PP           (1u << 9)
#define XHCI_PORTSC_SPEED_SHIFT  10u
#define XHCI_PORTSC_SPEED_MASK   0xFu
#define XHCI_PORTSC_CSC          (1u << 17)
#define XHCI_PORTSC_PRC          (1u << 21)
#define XHCI_PORTSC_RW1C (XHCI_PORTSC_CSC | (1u << 18) | (1u << 19) | (1u << 20) | XHCI_PORTSC_PRC | (1u << 22) | (1u << 23))

#define XHCI_CAP_DBOFF  0x14u
#define XHCI_CAP_RTSOFF 0x18u

#define XHCI_INTR_BASE 0x20u
#define XHCI_IMAN   0x00u
#define XHCI_ERSTSZ 0x08u
#define XHCI_ERSTBA 0x10u
#define XHCI_ERDP   0x18u
#define XHCI_IMAN_IE (1u << 1)

#define XHCI_TRB_CYCLE 1u
#define XHCI_TRB_TC    (1u << 1)
#define XHCI_TRB_IOC   (1u << 5)
#define XHCI_TRB_IDT   (1u << 6)
#define XHCI_TRB_DIR   (1u << 16)

#define XHCI_TRB_TYPE_NORMAL            1u
#define XHCI_TRB_TYPE_SETUP_STAGE       2u
#define XHCI_TRB_TYPE_DATA_STAGE        3u
#define XHCI_TRB_TYPE_STATUS_STAGE      4u
#define XHCI_TRB_TYPE_LINK              6u
#define XHCI_TRB_TYPE_ENABLE_SLOT       9u
#define XHCI_TRB_TYPE_DISABLE_SLOT     10u
#define XHCI_TRB_TYPE_ADDRESS_DEVICE   11u
#define XHCI_TRB_TYPE_CONFIGURE_EP     12u
#define XHCI_TRB_TYPE_TRANSFER_EVENT   32u
#define XHCI_TRB_TYPE_CMD_COMPLETION   33u
#define XHCI_TRB_TYPE_PORTSC_EVENT     34u

#define XHCI_COMP_SUCCESS 1u
#define XHCI_EP_TYPE_CONTROL 4u
#define XHCI_EP_TYPE_INTERRUPT_IN 7u
#define XHCI_WAIT_SPIN_SHORT 200000u
#define XHCI_WAIT_SPIN_CMD 600000u

#define USB_REQ_GET_DESCRIPTOR 6u
#define USB_REQ_SET_CONFIGURATION 9u
#define USB_REQ_SET_IDLE 10u
#define USB_DT_DEVICE 1u
#define USB_DT_CONFIG 2u
#define USB_CLASS_HID 3u

typedef struct __attribute__((packed)) {
    uint32_t lo;
    uint32_t hi;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

typedef struct __attribute__((packed)) {
    uint32_t seg_lo;
    uint32_t seg_hi;
    uint32_t seg_size;
    uint32_t rsvd;
} xhci_erst_ent_t;

typedef struct __attribute__((packed)) {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_pkt_t;

typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} usb_dev_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} usb_cfg_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} usb_if_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} usb_ep_desc_t;

static int xhci_enumerate_root_port(xhci_controller_t *xc, uint8_t port_id);

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr = 0x80000000u |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) |
                    ((uint32_t)off & 0xFCu);
    outportl(PCI_CFG_ADDR_PORT, addr);
    return inportl(PCI_CFG_DATA_PORT);
}

static void pci_cfg_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v) {
    uint32_t addr = 0x80000000u |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) |
                    ((uint32_t)off & 0xFCu);
    outportl(PCI_CFG_ADDR_PORT, addr);
    outportl(PCI_CFG_DATA_PORT, v);
}

static uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_cfg_read32(bus, slot, func, (uint8_t)(off & 0xFCu));
    return (uint16_t)((v >> ((off & 2u) * 8u)) & 0xFFFFu);
}

static void pci_cfg_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t val) {
    uint32_t cur = pci_cfg_read32(bus, slot, func, (uint8_t)(off & 0xFCu));
    uint32_t sh = (uint32_t)(off & 2u) * 8u;
    cur = (cur & ~(0xFFFFu << sh)) | ((uint32_t)val << sh);
    pci_cfg_write32(bus, slot, func, (uint8_t)(off & 0xFCu), cur);
}

static inline uint8_t mmio_read8(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint8_t *)(base + off);
}

static inline uint32_t mmio_read32(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static inline void mmio_write32(volatile uint8_t *base, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(base + off) = v;
}

static inline uint64_t mmio_read64(volatile uint8_t *base, uint32_t off) {
    uint64_t lo = (uint64_t)mmio_read32(base, off);
    uint64_t hi = (uint64_t)mmio_read32(base, off + 4u);
    return lo | (hi << 32);
}

static inline void mmio_write64(volatile uint8_t *base, uint32_t off, uint64_t v) {
    mmio_write32(base, off, (uint32_t)(v & 0xFFFFFFFFu));
    mmio_write32(base, off + 4u, (uint32_t)(v >> 32));
}

static int spin_wait_u32(volatile uint8_t *base, uint32_t off, uint32_t mask, uint32_t want_set, uint32_t loops) {
    for (uint32_t i = 0; i < loops; ++i) {
        uint32_t v = mmio_read32(base, off);
        if (((v & mask) != 0) == (want_set != 0)) return 0;
    }
    return -1;
}

static uint32_t xhci_ctx_bytes(const xhci_controller_t *xc) {
    return (xc && xc->ctx_sz64) ? 64u : 32u;
}

static uint8_t xhci_port_speed(const xhci_controller_t *xc, uint8_t port_id) {
    uint32_t off;
    uint32_t psc;
    if (!xc || !xc->op || port_id == 0 || port_id > xc->max_ports) return 0;
    off = XHCI_PORTSC_BASE + (uint32_t)(port_id - 1u) * XHCI_PORTSC_STRIDE;
    psc = mmio_read32(xc->op, off);
    return (uint8_t)((psc >> XHCI_PORTSC_SPEED_SHIFT) & XHCI_PORTSC_SPEED_MASK);
}

static uint16_t xhci_default_ep0_mps(uint8_t speed_id) {
    if (speed_id >= 4u) return 512u;
    if (speed_id == 3u) return 64u;
    return 8u;
}

static uint8_t xhci_interval_from_binterval(uint8_t speed_id, uint8_t bInterval) {
    uint8_t v;
    uint8_t p;
    if (bInterval == 0) bInterval = 1;
    if (speed_id >= 3u) {
        v = (uint8_t)(bInterval - 1u);
        if (v > 15u) v = 15u;
        return v;
    }
    p = 1u;
    v = 0u;
    while (p < bInterval && v < 15u) {
        p <<= 1;
        v++;
    }
    if (v < 3u) v = 3u;
    return v;
}

static void xhci_ring_init(usb_dma_block_t *ring, uint8_t *enq, uint8_t *ccs) {
    xhci_trb_t *trbs;
    uint32_t n;
    if (!ring || !ring->vaddr || ring->size < sizeof(xhci_trb_t) * 2u) return;
    trbs = (xhci_trb_t *)ring->vaddr;
    n = ring->size / sizeof(xhci_trb_t);
    memset(trbs, 0, ring->size);
    trbs[n - 1u].lo = ring->paddr;
    trbs[n - 1u].hi = 0;
    trbs[n - 1u].status = 0;
    trbs[n - 1u].control = ((uint32_t)XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_TC | XHCI_TRB_CYCLE;
    if (enq) *enq = 0;
    if (ccs) *ccs = 1;
}

static uint64_t xhci_ring_enqueue(usb_dma_block_t *ring, uint8_t *enq, uint8_t *ccs,
                                  uint32_t lo, uint32_t hi, uint32_t st, uint32_t ctrl) {
    xhci_trb_t *trbs;
    uint32_t n, idx;
    uint64_t phys;
    if (!ring || !ring->vaddr || !enq || !ccs) return 0;
    trbs = (xhci_trb_t *)ring->vaddr;
    n = ring->size / sizeof(xhci_trb_t);
    if (n < 2u) return 0;
    idx = *enq;
    if (idx >= n - 1u) idx = 0;
    phys = (uint64_t)ring->paddr + (uint64_t)idx * sizeof(xhci_trb_t);
    trbs[idx].lo = lo;
    trbs[idx].hi = hi;
    trbs[idx].status = st;
    trbs[idx].control = (ctrl & ~1u) | (uint32_t)(*ccs & 1u);
    idx++;
    if (idx >= n - 1u) {
        trbs[n - 1u].control = ((uint32_t)XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_TC | (uint32_t)(*ccs & 1u);
        idx = 0;
        *ccs ^= 1u;
    }
    *enq = (uint8_t)idx;
    return phys;
}

static void xhci_ring_doorbell(const xhci_controller_t *xc, uint8_t db, uint32_t val) {
    if (!xc || !xc->db) return;
    mmio_write32(xc->db, (uint32_t)db * 4u, val);
}

static void xhci_legacy_handoff(xhci_controller_t *xc) {
    uint32_t off;
    if (!xc || !xc->mmio || xc->ext_cap_off == 0) return;
    off = (uint32_t)xc->ext_cap_off;
    for (int guard = 0; guard < 32 && off >= 0x40u; ++guard) {
        uint32_t cap = mmio_read32(xc->mmio, off);
        uint8_t id = (uint8_t)(cap & 0xFFu);
        uint8_t next = (uint8_t)((cap >> 8) & 0xFFu);
        if (id == 1u) {
            uint32_t sem = cap;
            if (sem & (1u << 16)) {
                sem |= (1u << 24);
                mmio_write32(xc->mmio, off, sem);
                for (uint32_t i = 0; i < 500000; ++i) {
                    uint32_t cur = mmio_read32(xc->mmio, off);
                    if ((cur & (1u << 16)) == 0) break;
                }
            }
            return;
        }
        if (next == 0) break;
        off += ((uint32_t)next << 2);
    }
}

static void xhci_port_power_and_reset(xhci_controller_t *xc, uint8_t port_index) {
    uint32_t off;
    uint32_t v;
    if (!xc || !xc->op) return;
    off = XHCI_PORTSC_BASE + (uint32_t)port_index * XHCI_PORTSC_STRIDE;
    v = mmio_read32(xc->op, off);
    if (v & XHCI_PORTSC_RW1C) mmio_write32(xc->op, off, (v & XHCI_PORTSC_RW1C));
    v = mmio_read32(xc->op, off);
    mmio_write32(xc->op, off, (v & ~XHCI_PORTSC_RW1C) | XHCI_PORTSC_PP);
    for (volatile int d = 0; d < 200000; ++d) { (void)d; }
    v = mmio_read32(xc->op, off);
    if ((v & XHCI_PORTSC_CCS) == 0) return;
    mmio_write32(xc->op, off, (v & ~XHCI_PORTSC_RW1C) | XHCI_PORTSC_PP | XHCI_PORTSC_PR);
    (void)spin_wait_u32(xc->op, off, XHCI_PORTSC_PR, 0, 2000000);
    v = mmio_read32(xc->op, off);
    if (v & XHCI_PORTSC_RW1C) mmio_write32(xc->op, off, (v & XHCI_PORTSC_RW1C));
}

static int xhci_setup_rings(xhci_controller_t *xc) {
    xhci_erst_ent_t *erst;
    if (!xc) return -1;
    xc->cmd_ring_size = 64;
    xc->evt_ring_size = 256;
    xc->evt_deq = 0;
    xc->evt_ccs = 1;
    xc->cmd_wait_ptr = 0;
    xc->cmd_wait_done = 0;
    xc->xfer_wait_ptr = 0;
    xc->xfer_wait_done = 0;

    if (usb_dma_alloc_zero(256u * 8u, 64u, &xc->dcbaa) < 0) return -1;
    if (usb_dma_alloc_zero(xc->cmd_ring_size * sizeof(xhci_trb_t), 64u, &xc->cmd_ring) < 0) return -1;
    if (usb_dma_alloc_zero(xc->evt_ring_size * sizeof(xhci_trb_t), 64u, &xc->evt_ring) < 0) return -1;
    if (usb_dma_alloc_zero(sizeof(xhci_erst_ent_t), 64u, &xc->erst) < 0) return -1;

    xhci_ring_init(&xc->cmd_ring, &xc->cmd_enq, &xc->cmd_ccs);
    memset(xc->evt_ring.vaddr, 0, xc->evt_ring.size);
    memset(xc->erst.vaddr, 0, xc->erst.size);
    erst = (xhci_erst_ent_t *)xc->erst.vaddr;
    erst[0].seg_lo = xc->evt_ring.paddr;
    erst[0].seg_hi = 0;
    erst[0].seg_size = xc->evt_ring_size;
    return 0;
}

static void xhci_program_runtime(xhci_controller_t *xc) {
    uint32_t intr_off;
    uint64_t erdp;
    if (!xc || !xc->rt) return;
    intr_off = XHCI_INTR_BASE;
    mmio_write32(xc->rt, intr_off + XHCI_ERSTSZ, 1u);
    mmio_write64(xc->rt, intr_off + XHCI_ERSTBA, (uint64_t)xc->erst.paddr);
    erdp = (uint64_t)xc->evt_ring.paddr;
    mmio_write64(xc->rt, intr_off + XHCI_ERDP, erdp | (1ull << 3));
    mmio_write32(xc->rt, intr_off + XHCI_IMAN, XHCI_IMAN_IE);
}

static int xhci_cmd_submit_wait(xhci_controller_t *xc, uint32_t lo, uint32_t hi, uint32_t st, uint32_t ctrl, uint8_t *slot_out);
static void xhci_poll_events(xhci_controller_t *xc);

static int xhci_wait_transfer(xhci_controller_t *xc, uint64_t wait_ptr, uint8_t slot_id, uint8_t ep_dci, uint8_t *cc_out) {
    if (!xc || wait_ptr == 0) return -1;
    xc->xfer_wait_ptr = wait_ptr;
    xc->xfer_wait_done = 0;
    xc->xfer_wait_cc = 0;
    xc->xfer_wait_slot = slot_id;
    xc->xfer_wait_ep = ep_dci;
    for (uint32_t i = 0; i < XHCI_WAIT_SPIN_SHORT; ++i) {
        xhci_poll_events(xc);
        if (xc->xfer_wait_done) break;
    }
    xc->xfer_wait_ptr = 0;
    if (!xc->xfer_wait_done) {
        printf("[usb][xhci] transfer timeout ptr=0x%x\n", (uint32_t)wait_ptr);
        return -1;
    }
    if (cc_out) *cc_out = xc->xfer_wait_cc;
    return (xc->xfer_wait_cc == XHCI_COMP_SUCCESS) ? 0 : -1;
}

static xhci_slot_state_t *xhci_slot_get(xhci_controller_t *xc, uint8_t slot_id) {
    if (!xc || slot_id == 0 || slot_id > XHCI_MAX_TRACKED_SLOTS) return 0;
    return &xc->slots[slot_id];
}

static void xhci_build_address_input_ctx(xhci_controller_t *xc, xhci_slot_state_t *slot) {
    uint32_t ctx_bytes;
    uint32_t *input_ctrl;
    uint32_t *slot_ctx;
    uint32_t *ep0_ctx;
    uint64_t ep_ring;
    if (!xc || !slot || !slot->input_ctx.vaddr || !slot->device_ctx.vaddr) return;
    ctx_bytes = xhci_ctx_bytes(xc);
    memset(slot->input_ctx.vaddr, 0, slot->input_ctx.size);
    memset(slot->device_ctx.vaddr, 0, slot->device_ctx.size);
    input_ctrl = (uint32_t *)slot->input_ctx.vaddr;
    slot_ctx = (uint32_t *)((uint8_t *)slot->input_ctx.vaddr + ctx_bytes);
    ep0_ctx = (uint32_t *)((uint8_t *)slot->input_ctx.vaddr + 2u * ctx_bytes);
    input_ctrl[1] = (1u << 0) | (1u << 1);
    slot_ctx[0] = ((uint32_t)(slot->speed_id & 0xFu) << 20) | (1u << 27);
    slot_ctx[1] = ((uint32_t)slot->port_id << 16);
    ep_ring = (uint64_t)slot->ep0_ring.paddr;
    /* Endpoint Context layout:
     * DW1: MaxPacketSize[31:16], EPType[5:3], CErr[2:1]
     * DW2/3: TR Dequeue Pointer (DCS in bit0 of DW2)
     * DW4: Average TRB Length
     */
    ep0_ctx[1] = ((uint32_t)slot->max_packet0 << 16) | (3u << 1) | ((uint32_t)XHCI_EP_TYPE_CONTROL << 3);
    ep0_ctx[2] = (uint32_t)(ep_ring & ~0xFULL) | 1u;
    ep0_ctx[3] = (uint32_t)(ep_ring >> 32);
    ep0_ctx[4] = 8u;
}

static void xhci_build_config_input_ctx(xhci_controller_t *xc, xhci_slot_state_t *slot, int add_ep0_flag) {
    uint32_t ctx_bytes;
    uint32_t *input_ctrl;
    uint32_t *slot_ctx;
    uint32_t *ep0_ctx;
    uint32_t *hid_ep_ctx;
    uint32_t *out_slot_ctx;
    uint32_t *out_ep0_ctx;
    uint64_t intr_ring;
    uint8_t dci;
    uint32_t ctx_entries;
    uint8_t interval;
    if (!xc || !slot || !slot->input_ctx.vaddr) return;
    ctx_bytes = xhci_ctx_bytes(xc);
    dci = slot->hid_ep_dci;
    if (dci == 0u) return;
    memset(slot->input_ctx.vaddr, 0, slot->input_ctx.size);
    input_ctrl = (uint32_t *)slot->input_ctx.vaddr;
    slot_ctx = (uint32_t *)((uint8_t *)slot->input_ctx.vaddr + ctx_bytes);
    ep0_ctx = (uint32_t *)((uint8_t *)slot->input_ctx.vaddr + 2u * ctx_bytes);
    hid_ep_ctx = (uint32_t *)((uint8_t *)slot->input_ctx.vaddr + ((uint32_t)1u + dci) * ctx_bytes);
    out_slot_ctx = (uint32_t *)((uint8_t *)slot->device_ctx.vaddr + ctx_bytes);
    out_ep0_ctx = (uint32_t *)((uint8_t *)slot->device_ctx.vaddr + 2u * ctx_bytes);
    memcpy(slot_ctx, out_slot_ctx, ctx_bytes);
    memcpy(ep0_ctx, out_ep0_ctx, ctx_bytes);
    /* Configure Endpoint: include slot + target endpoint; EP0 add-flag is controller-specific. */
    input_ctrl[1] = (1u << 0) | (1u << dci);
    if (add_ep0_flag) input_ctrl[1] |= (1u << 1);
    ctx_entries = (slot_ctx[0] >> 27) & 0x1Fu;
    if (ctx_entries < dci) ctx_entries = dci;
    slot_ctx[0] &= ~(0x1Fu << 27);
    slot_ctx[0] |= (ctx_entries & 0x1Fu) << 27;
    intr_ring = (uint64_t)slot->intr_ring.paddr;
    interval = xhci_interval_from_binterval(slot->speed_id, slot->hid_interval);
    hid_ep_ctx[0] = ((uint32_t)interval << 16);
    /* CErr is only meaningful for control/bulk; keep it 0 for interrupt endpoints. */
    hid_ep_ctx[1] = ((uint32_t)slot->hid_max_packet << 16) | ((uint32_t)XHCI_EP_TYPE_INTERRUPT_IN << 3);
    hid_ep_ctx[2] = (uint32_t)(intr_ring & ~0xFULL) | 1u;
    hid_ep_ctx[3] = (uint32_t)(intr_ring >> 32);
    hid_ep_ctx[4] = ((uint32_t)slot->hid_max_packet << 16) | (uint32_t)slot->hid_max_packet;
}

static int xhci_control_transfer(xhci_controller_t *xc, xhci_slot_state_t *slot,
                                 uint8_t bmRequestType, uint8_t bRequest,
                                 uint16_t wValue, uint16_t wIndex,
                                 void *buf, uint16_t len) {
    usb_setup_pkt_t setup;
    uint64_t setup_ptr, data_ptr = 0, status_ptr;
    int dir_in = (bmRequestType & 0x80u) ? 1 : 0;
    uint8_t cc = 0;
    if (!xc || !slot || !slot->online) return -1;
    setup.bmRequestType = bmRequestType;
    setup.bRequest = bRequest;
    setup.wValue = wValue;
    setup.wIndex = wIndex;
    setup.wLength = len;

    if (len > 0 && buf && !dir_in) {
        memcpy(slot->ctrl_buf.vaddr, buf, len);
    }

    setup_ptr = xhci_ring_enqueue(&slot->ep0_ring, &slot->ep0_enq, &slot->ep0_ccs,
                                  *(uint32_t *)&setup, *(((uint32_t *)&setup) + 1),
                                  8u, ((uint32_t)XHCI_TRB_TYPE_SETUP_STAGE << 10) | XHCI_TRB_IDT |
                                  ((len == 0) ? 0u : (dir_in ? (3u << 16) : (2u << 16))));
    if (setup_ptr == 0) return -1;

    if (len > 0) {
        data_ptr = xhci_ring_enqueue(&slot->ep0_ring, &slot->ep0_enq, &slot->ep0_ccs,
                                     slot->ctrl_buf.paddr, 0,
                                     (uint32_t)len,
                                     ((uint32_t)XHCI_TRB_TYPE_DATA_STAGE << 10) |
                                     (dir_in ? XHCI_TRB_DIR : 0u));
        if (data_ptr == 0) return -1;
    }

    status_ptr = xhci_ring_enqueue(&slot->ep0_ring, &slot->ep0_enq, &slot->ep0_ccs,
                                   0, 0, 0,
                                   ((uint32_t)XHCI_TRB_TYPE_STATUS_STAGE << 10) |
                                   XHCI_TRB_IOC | (dir_in ? 0u : XHCI_TRB_DIR));
    if (status_ptr == 0) return -1;
    xhci_ring_doorbell(xc, slot->slot_id, 1u);
    if (xhci_wait_transfer(xc, status_ptr, slot->slot_id, 1u, &cc) < 0) return -1;

    if (len > 0 && buf && dir_in) {
        memcpy(buf, slot->ctrl_buf.vaddr, len);
    }
    return 0;
}

static int xhci_hid_rearm_interrupt(xhci_controller_t *xc, xhci_slot_state_t *slot) {
    uint64_t trb;
    if (!xc || !slot || !slot->hid_ready || !slot->intr_buf.vaddr) return -1;
    trb = xhci_ring_enqueue(&slot->intr_ring, &slot->intr_enq, &slot->intr_ccs,
                            slot->intr_buf.paddr, 0,
                            slot->hid_max_packet,
                            ((uint32_t)XHCI_TRB_TYPE_NORMAL << 10) | XHCI_TRB_IOC);
    if (trb == 0) return -1;
    slot->intr_pending_trb = trb;
    xhci_ring_doorbell(xc, slot->slot_id, (uint32_t)slot->hid_ep_dci);
    return 0;
}

static int xhci_parse_hid_config(xhci_slot_state_t *slot, const uint8_t *cfg, uint16_t len, uint8_t *cfgval_out) {
    uint16_t off = 0;
    int in_hid_if = 0;
    uint8_t ifnum = 0;
    if (!slot || !cfg || len < sizeof(usb_cfg_desc_t)) return -1;
    if (cfgval_out) *cfgval_out = ((const usb_cfg_desc_t *)cfg)->bConfigurationValue;
    slot->hid_ready = 0;
    while (off + 2 <= len) {
        uint8_t dlen = cfg[off];
        uint8_t dtype = cfg[off + 1];
        if (dlen < 2 || off + dlen > len) break;
        if (dtype == 4 && dlen >= sizeof(usb_if_desc_t)) {
            const usb_if_desc_t *id = (const usb_if_desc_t *)(cfg + off);
            ifnum = id->bInterfaceNumber;
            in_hid_if = (id->bInterfaceClass == USB_CLASS_HID &&
                         id->bInterfaceSubClass == 1 && id->bInterfaceProtocol == 2);
        } else if (dtype == 5 && dlen >= sizeof(usb_ep_desc_t) && in_hid_if) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)(cfg + off);
            if ((ep->bEndpointAddress & 0x80u) && ((ep->bmAttributes & 0x03u) == 0x03u)) {
                uint8_t epn = ep->bEndpointAddress & 0x0Fu;
                slot->hid_iface = ifnum;
                slot->hid_ep_addr = ep->bEndpointAddress;
                slot->hid_ep_dci = (uint8_t)(epn * 2u + 1u);
                slot->hid_interval = ep->bInterval ? ep->bInterval : 10u;
                slot->hid_max_packet = (uint16_t)(ep->wMaxPacketSize & 0x07FFu);
                if (slot->hid_max_packet == 0) slot->hid_max_packet = 8;
                if (slot->hid_max_packet > 64) slot->hid_max_packet = 64;
                return 0;
            }
        }
        off += dlen;
    }
    return -1;
}

static int xhci_configure_hid(xhci_controller_t *xc, xhci_slot_state_t *slot) {
    usb_dev_desc_t dd;
    uint8_t cfg_hdr[9];
    uint8_t cfg_buf[256];
    uint16_t total;
    uint8_t cfgval = 1;
    uint64_t *dcbaa;
    if (!xc || !slot) return -1;
    memset(&dd, 0, sizeof(dd));
    if (xhci_control_transfer(xc, slot, 0x80u, USB_REQ_GET_DESCRIPTOR, (uint16_t)(USB_DT_DEVICE << 8), 0, &dd, sizeof(dd)) < 0) {
        printf("[usb][xhci] slot=%u GET_DESCRIPTOR(device) failed\n", (uint32_t)slot->slot_id);
        return -1;
    }
    memset(cfg_hdr, 0, sizeof(cfg_hdr));
    if (xhci_control_transfer(xc, slot, 0x80u, USB_REQ_GET_DESCRIPTOR, (uint16_t)(USB_DT_CONFIG << 8), 0, cfg_hdr, sizeof(cfg_hdr)) < 0) {
        printf("[usb][xhci] slot=%u GET_DESCRIPTOR(config hdr) failed\n", (uint32_t)slot->slot_id);
        return -1;
    }
    total = (uint16_t)cfg_hdr[2] | ((uint16_t)cfg_hdr[3] << 8);
    if (total < sizeof(cfg_hdr)) total = sizeof(cfg_hdr);
    if (total > sizeof(cfg_buf)) total = sizeof(cfg_buf);
    memset(cfg_buf, 0, sizeof(cfg_buf));
    if (xhci_control_transfer(xc, slot, 0x80u, USB_REQ_GET_DESCRIPTOR, (uint16_t)(USB_DT_CONFIG << 8), 0, cfg_buf, total) < 0) {
        printf("[usb][xhci] slot=%u GET_DESCRIPTOR(config %u) failed\n",
               (uint32_t)slot->slot_id, (uint32_t)total);
        return -1;
    }
    if (xhci_parse_hid_config(slot, cfg_buf, total, &cfgval) < 0) {
        printf("[usb][xhci] slot=%u no HID boot mouse interface found\n", (uint32_t)slot->slot_id);
        return -1;
    }

    if (!slot->intr_ring.vaddr && usb_dma_alloc_zero(32u * sizeof(xhci_trb_t), 64u, &slot->intr_ring) < 0) return -1;
    if (!slot->intr_buf.vaddr && usb_dma_alloc_zero(slot->hid_max_packet, 64u, &slot->intr_buf) < 0) return -1;
    xhci_ring_init(&slot->intr_ring, &slot->intr_enq, &slot->intr_ccs);
    xhci_build_config_input_ctx(xc, slot, 0);
    dcbaa = (uint64_t *)xc->dcbaa.vaddr;
    dcbaa[slot->slot_id] = (uint64_t)slot->device_ctx.paddr;
    printf("[usb][xhci] cfgep slot=%u dci=%u interval=%u mps=%u ring=0x%x mode=%u\n",
           (uint32_t)slot->slot_id, (uint32_t)slot->hid_ep_dci, (uint32_t)slot->hid_interval,
           (uint32_t)slot->hid_max_packet, slot->intr_ring.paddr, 0u);

    if (xhci_cmd_submit_wait(xc, slot->input_ctx.paddr, 0, 0,
                             ((uint32_t)XHCI_TRB_TYPE_CONFIGURE_EP << 10) | ((uint32_t)slot->slot_id << 24), 0) < 0) {
        xhci_build_config_input_ctx(xc, slot, 1);
        printf("[usb][xhci] cfgep slot=%u dci=%u interval=%u mps=%u ring=0x%x mode=%u\n",
               (uint32_t)slot->slot_id, (uint32_t)slot->hid_ep_dci, (uint32_t)slot->hid_interval,
               (uint32_t)slot->hid_max_packet, slot->intr_ring.paddr, 1u);
        if (xhci_cmd_submit_wait(xc, slot->input_ctx.paddr, 0, 0,
                                 ((uint32_t)XHCI_TRB_TYPE_CONFIGURE_EP << 10) | ((uint32_t)slot->slot_id << 24), 0) < 0) {
            printf("[usb][xhci] slot=%u Configure Endpoint failed\n", (uint32_t)slot->slot_id);
            return -1;
        }
    }
    if (xhci_control_transfer(xc, slot, 0x00u, USB_REQ_SET_CONFIGURATION, cfgval, 0, 0, 0) < 0) return -1;
    (void)xhci_control_transfer(xc, slot, 0x21u, 0x0Bu, 0u, slot->hid_iface, 0, 0); /* SET_PROTOCOL boot */
    (void)xhci_control_transfer(xc, slot, 0x21u, USB_REQ_SET_IDLE, 0u, slot->hid_iface, 0, 0);
    slot->hid_ready = 1;
    printf("[usb][xhci] HID device configured slot=%u iface=%u ep=0x%02x mps=%u interval=%u\n",
           (uint32_t)slot->slot_id, (uint32_t)slot->hid_iface, (uint32_t)slot->hid_ep_addr,
           (uint32_t)slot->hid_max_packet, (uint32_t)slot->hid_interval);
    if (xhci_hid_rearm_interrupt(xc, slot) == 0) {
        printf("[usb][xhci] interrupt endpoint active slot=%u dci=%u\n",
               (uint32_t)slot->slot_id, (uint32_t)slot->hid_ep_dci);
    }
    return 0;
}

static int xhci_disable_slot(xhci_controller_t *xc, uint8_t slot_id) {
    if (!xc || slot_id == 0) return -1;
    return xhci_cmd_submit_wait(xc, 0, 0, 0,
                                ((uint32_t)XHCI_TRB_TYPE_DISABLE_SLOT << 10) | ((uint32_t)slot_id << 24), 0);
}

static void xhci_mark_port_disconnected(xhci_controller_t *xc, uint8_t port_id) {
    uint8_t slot_id;
    xhci_slot_state_t *slot;
    if (!xc || port_id == 0 || port_id > xc->max_ports) return;
    slot_id = xc->port_to_slot[port_id];
    if (slot_id == 0) return;
    slot = xhci_slot_get(xc, slot_id);
    if (slot) slot->online = 0;
    (void)xhci_disable_slot(xc, slot_id);
    xc->port_to_slot[port_id] = 0;
    printf("[usb][xhci] device disconnected port=%u slot=%u\n", (uint32_t)port_id, (uint32_t)slot_id);
}

static int xhci_enumerate_root_port(xhci_controller_t *xc, uint8_t port_id) {
    uint32_t psc;
    uint8_t speed;
    uint8_t slot_id = 0;
    xhci_slot_state_t *slot;
    uint64_t *dcbaa;
    uint32_t ctx_bytes;
    if (!xc || !xc->op || port_id == 0 || port_id > xc->max_ports) return -1;
    psc = mmio_read32(xc->op, XHCI_PORTSC_BASE + (uint32_t)(port_id - 1u) * XHCI_PORTSC_STRIDE);
    if ((psc & XHCI_PORTSC_CCS) == 0) return 0;
    if (xc->port_to_slot[port_id] != 0) {
        slot = xhci_slot_get(xc, xc->port_to_slot[port_id]);
        if (slot && slot->online) return 0;
    }
    if (xhci_cmd_submit_wait(xc, 0, 0, 0, (uint32_t)XHCI_TRB_TYPE_ENABLE_SLOT << 10, &slot_id) < 0) return -1;
    slot = xhci_slot_get(xc, slot_id);
    if (!slot) return -1;
    memset(slot, 0, sizeof(*slot));
    speed = xhci_port_speed(xc, port_id);
    slot->used = 1;
    slot->online = 1;
    slot->slot_id = slot_id;
    slot->port_id = port_id;
    slot->speed_id = speed;
    slot->max_packet0 = xhci_default_ep0_mps(speed);
    xc->port_to_slot[port_id] = slot_id;
    ctx_bytes = xhci_ctx_bytes(xc);
    if (usb_dma_alloc_zero(ctx_bytes * 64u, 64u, &slot->input_ctx) < 0) goto fail;
    if (usb_dma_alloc_zero(ctx_bytes * 64u, 64u, &slot->device_ctx) < 0) goto fail;
    if (usb_dma_alloc_zero(32u * sizeof(xhci_trb_t), 64u, &slot->ep0_ring) < 0) goto fail;
    if (usb_dma_alloc_zero(1024u, 64u, &slot->ctrl_buf) < 0) goto fail;
    xhci_ring_init(&slot->ep0_ring, &slot->ep0_enq, &slot->ep0_ccs);
    xhci_build_address_input_ctx(xc, slot);
    dcbaa = (uint64_t *)xc->dcbaa.vaddr;
    dcbaa[slot_id] = (uint64_t)slot->device_ctx.paddr;
    if (xhci_cmd_submit_wait(xc, slot->input_ctx.paddr, 0, 0,
                             ((uint32_t)XHCI_TRB_TYPE_ADDRESS_DEVICE << 10) | ((uint32_t)slot_id << 24), 0) < 0) {
        goto fail;
    }
    printf("[usb][xhci] device connected on port %u slot=%u speed=%u\n",
           (uint32_t)port_id, (uint32_t)slot_id, (uint32_t)speed);
    (void)xhci_configure_hid(xc, slot);
    return 0;
fail:
    slot->online = 0;
    xc->port_to_slot[port_id] = 0;
    (void)xhci_disable_slot(xc, slot_id);
    return -1;
}

static void xhci_enumerate_root_ports(xhci_controller_t *xc) {
    if (!xc || !xc->op) return;
    for (uint8_t p = 1; p <= xc->max_ports; ++p) {
        (void)xhci_enumerate_root_port(xc, p);
    }
}

static void xhci_handle_transfer_event(xhci_controller_t *xc, xhci_trb_t *t) {
    uint8_t cc = (uint8_t)(t->status >> 24);
    uint8_t ep = (uint8_t)((t->control >> 16) & 0x1Fu);
    uint8_t slot_id = (uint8_t)(t->control >> 24);
    uint32_t rem = (t->status & 0x00FFFFFFu);
    uint64_t ptr = (uint64_t)t->lo | ((uint64_t)t->hi << 32);
    xhci_slot_state_t *slot = xhci_slot_get(xc, slot_id);
    if (xc->xfer_wait_ptr != 0 &&
        slot_id == xc->xfer_wait_slot &&
        ep == xc->xfer_wait_ep &&
        (ptr == xc->xfer_wait_ptr || cc != XHCI_COMP_SUCCESS)) {
        xc->xfer_wait_done = 1;
        xc->xfer_wait_cc = cc;
        xc->xfer_wait_slot = slot_id;
        xc->xfer_wait_ep = ep;
        return;
    }
    if (!slot || !slot->hid_ready || ep != slot->hid_ep_dci || ptr != slot->intr_pending_trb) return;
    if (cc == XHCI_COMP_SUCCESS) {
        uint16_t n = slot->hid_max_packet;
        if (rem <= n) n = (uint16_t)(n - rem);
        usb_hid_process_boot_report((const uint8_t *)slot->intr_buf.vaddr, n);
    }
    (void)xhci_hid_rearm_interrupt(xc, slot);
}

static void xhci_consume_events(xhci_controller_t *xc) {
    xhci_trb_t *evt;
    uint32_t budget = 128;
    if (!xc || !xc->rt || !xc->evt_ring.vaddr || xc->evt_ring_size == 0) return;
    evt = (xhci_trb_t *)xc->evt_ring.vaddr;
    while (budget--) {
        xhci_trb_t *t = &evt[xc->evt_deq];
        uint32_t ctrl = t->control;
        uint32_t type = (ctrl >> 10) & 0x3Fu;
        uint32_t c = ctrl & 1u;
        if (c != (uint32_t)(xc->evt_ccs & 1u)) break;
        if (type == XHCI_TRB_TYPE_PORTSC_EVENT) {
            uint8_t port = (uint8_t)((t->lo >> 24) & 0xFFu);
            if (port > 0 && xc->op) {
                uint32_t psc = mmio_read32(xc->op, XHCI_PORTSC_BASE + (uint32_t)(port - 1u) * XHCI_PORTSC_STRIDE);
                if ((psc & XHCI_PORTSC_CCS) == 0) xhci_mark_port_disconnected(xc, port);
                else if (psc & (XHCI_PORTSC_CSC | XHCI_PORTSC_PRC)) (void)xhci_enumerate_root_port(xc, port);
            }
        } else if (type == XHCI_TRB_TYPE_CMD_COMPLETION) {
            uint8_t cc = (uint8_t)(t->status >> 24);
            uint8_t slot_id = (uint8_t)(ctrl >> 24);
            uint64_t ptr = (uint64_t)t->lo | ((uint64_t)t->hi << 32);
            if (xc->cmd_wait_ptr != 0 && ptr == xc->cmd_wait_ptr) {
                xc->cmd_wait_done = 1;
                xc->cmd_wait_cc = cc;
                xc->cmd_wait_slot = slot_id;
            }
        } else if (type == XHCI_TRB_TYPE_TRANSFER_EVENT) {
            xhci_handle_transfer_event(xc, t);
        }
        xc->evt_deq++;
        if (xc->evt_deq >= xc->evt_ring_size) {
            xc->evt_deq = 0;
            xc->evt_ccs ^= 1u;
        }
    }
    {
        uint64_t erdp = (uint64_t)xc->evt_ring.paddr + (uint64_t)xc->evt_deq * sizeof(xhci_trb_t);
        mmio_write64(xc->rt, XHCI_INTR_BASE + XHCI_ERDP, erdp | (1ull << 3));
    }
}

static void xhci_poll_events(xhci_controller_t *xc) {
    uint32_t st;
    if (!xc || !xc->op) return;
    st = mmio_read32(xc->op, XHCI_USBSTS);
    if (st & XHCI_STS_EINT) mmio_write32(xc->op, XHCI_USBSTS, XHCI_STS_EINT);
    xhci_consume_events(xc);
}

static int xhci_cmd_submit_wait(xhci_controller_t *xc, uint32_t lo, uint32_t hi, uint32_t st, uint32_t ctrl, uint8_t *slot_out) {
    uint64_t ptr;
    if (!xc || !xc->running) return -1;
    ptr = xhci_ring_enqueue(&xc->cmd_ring, &xc->cmd_enq, &xc->cmd_ccs, lo, hi, st, ctrl);
    if (ptr == 0) return -1;
    xc->cmd_wait_ptr = ptr;
    xc->cmd_wait_done = 0;
    xc->cmd_wait_cc = 0;
    xc->cmd_wait_slot = 0;
    xhci_ring_doorbell(xc, 0, 0);
    for (uint32_t i = 0; i < XHCI_WAIT_SPIN_CMD; ++i) {
        xhci_poll_events(xc);
        if (xc->cmd_wait_done) break;
    }
    xc->cmd_wait_ptr = 0;
    if (!xc->cmd_wait_done) {
        printf("[usb][xhci] command timeout ctrl=0x%x\n", ctrl);
        return -1;
    }
    if (xc->cmd_wait_cc != XHCI_COMP_SUCCESS) {
        printf("[usb][xhci] command failed cc=%u ctrl=0x%x\n",
               (uint32_t)xc->cmd_wait_cc, ctrl);
        return -1;
    }
    if (slot_out) *slot_out = xc->cmd_wait_slot;
    return 0;
}

int xhci_init_controller(xhci_controller_t *xc,
                         uint8_t bus, uint8_t dev, uint8_t fn,
                         uint16_t vendor, uint16_t device,
                         uint32_t bar0, uint32_t bar1, uint8_t irq_line) {
    uint64_t mmio_base = 0;
    uint16_t cmd;
    uint32_t hcs1, hcc1;
    uint32_t dboff, rtsoff;
    uint32_t usbcmd;
    if (!xc) return -1;
    memset(xc, 0, sizeof(*xc));
    xc->bus = bus;
    xc->dev = dev;
    xc->fn = fn;
    xc->vendor = vendor;
    xc->device = device;
    xc->irq_line = irq_line;
    if ((bar0 & 1u) != 0) return -1;
    if ((bar0 & 0x6u) == 0x4u) mmio_base = (((uint64_t)bar1) << 32) | (uint64_t)(bar0 & ~0xFULL);
    else mmio_base = (uint64_t)(bar0 & ~0xFULL);
    if (mmio_base == 0 || mmio_base > 0x00000000FFFFFFFFULL) return -1;
    cmd = pci_cfg_read16(bus, dev, fn, 0x04);
    cmd |= 0x0002u;
    cmd |= 0x0004u;
    pci_cfg_write16(bus, dev, fn, 0x04, cmd);
    xc->mmio_base = mmio_base;
    xc->mmio = (volatile uint8_t *)(uintptr_t)mmio_base;
    xc->cap_len = mmio_read8(xc->mmio, 0x00);
    hcs1 = mmio_read32(xc->mmio, 0x04);
    hcc1 = mmio_read32(xc->mmio, 0x10);
    dboff = mmio_read32(xc->mmio, XHCI_CAP_DBOFF) & ~0x3u;
    rtsoff = mmio_read32(xc->mmio, XHCI_CAP_RTSOFF) & ~0x1Fu;
    xc->max_ports = (uint8_t)((hcs1 >> 24) & 0xFFu);
    xc->max_slots = (uint8_t)(hcs1 & 0xFFu);
    xc->ext_cap_off = (uint8_t)((hcc1 >> 16) & 0xFFu);
    xc->ctx_sz64 = (uint8_t)((hcc1 >> 2) & 1u);
    xc->op = xc->mmio + xc->cap_len;
    xc->rt = xc->mmio + rtsoff;
    xc->db = xc->mmio + dboff;
    xhci_legacy_handoff(xc);
    usbcmd = mmio_read32(xc->op, XHCI_USBCMD);
    usbcmd &= ~XHCI_CMD_RUN;
    mmio_write32(xc->op, XHCI_USBCMD, usbcmd);
    (void)spin_wait_u32(xc->op, XHCI_USBSTS, XHCI_STS_HCH, 1, 2000000);
    usbcmd = mmio_read32(xc->op, XHCI_USBCMD);
    usbcmd |= XHCI_CMD_HCRST;
    mmio_write32(xc->op, XHCI_USBCMD, usbcmd);
    if (spin_wait_u32(xc->op, XHCI_USBCMD, XHCI_CMD_HCRST, 0, 4000000) < 0) return -1;
    (void)spin_wait_u32(xc->op, XHCI_USBSTS, XHCI_STS_HCH, 1, 2000000);
    (void)spin_wait_u32(xc->op, XHCI_USBSTS, XHCI_STS_CNR, 0, 4000000);
    if (xhci_setup_rings(xc) < 0) return -1;
    mmio_write32(xc->op, XHCI_DNCTRL, 0);
    mmio_write64(xc->op, XHCI_DCBAAP, (uint64_t)xc->dcbaa.paddr);
    mmio_write64(xc->op, XHCI_CRCR, ((uint64_t)xc->cmd_ring.paddr & ~0x3FULL) | 1u);
    mmio_write32(xc->op, XHCI_CONFIG, (uint32_t)xc->max_slots);
    xhci_program_runtime(xc);
    for (uint8_t p = 0; p < xc->max_ports; ++p) xhci_port_power_and_reset(xc, p);
    usbcmd = mmio_read32(xc->op, XHCI_USBCMD);
    usbcmd |= XHCI_CMD_RUN | XHCI_CMD_INTE;
    mmio_write32(xc->op, XHCI_USBCMD, usbcmd);
    xc->running = 1;
    xc->used = 1;
    printf("[usb][xhci] controller initialized %u:%u.%u ports=%u slots=%u mmio=0x%x\n",
           (uint32_t)bus, (uint32_t)dev, (uint32_t)fn,
           (uint32_t)xc->max_ports, (uint32_t)xc->max_slots, (uint32_t)xc->mmio_base);
    xhci_enumerate_root_ports(xc);
    return 0;
}

void xhci_poll_controller(xhci_controller_t *xc) {
    if (!xc || !xc->used || !xc->running || !xc->op) return;
    xhci_poll_events(xc);
    for (uint8_t p = 0; p < xc->max_ports; ++p) {
        uint32_t off = XHCI_PORTSC_BASE + (uint32_t)p * XHCI_PORTSC_STRIDE;
        uint32_t v = mmio_read32(xc->op, off);
        uint32_t ch = v & XHCI_PORTSC_RW1C;
        uint8_t port_id = (uint8_t)(p + 1u);
        if ((v & XHCI_PORTSC_CCS) == 0 && xc->port_to_slot[port_id] != 0) xhci_mark_port_disconnected(xc, port_id);
        else if ((v & XHCI_PORTSC_CCS) != 0 && (ch & (XHCI_PORTSC_CSC | XHCI_PORTSC_PRC)) != 0) (void)xhci_enumerate_root_port(xc, port_id);
        if (ch) mmio_write32(xc->op, off, ch);
    }
}

void xhci_debug_dump(const xhci_controller_t *xc) {
    if (!xc || !xc->used || !xc->op) return;
    printf("[usb][xhci] %u:%u.%u ports=%u slots=%u run=%d\n",
           (uint32_t)xc->bus, (uint32_t)xc->dev, (uint32_t)xc->fn,
           (uint32_t)xc->max_ports, (uint32_t)xc->max_slots, xc->running);
}
