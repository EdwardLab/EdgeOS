#include "drivers/uhci.h"

#include "drivers/usb_dma.h"
#include "io_ports.h"
#include "string.h"
#include "stdio.h"

#define UHCI_FRAME_COUNT 1024u
#define UHCI_FRAME_ALIGN 4096u
#define UHCI_QH_ALIGN    16u
#define UHCI_TD_ALIGN    16u
#define UHCI_QH_POOL_COUNT 64u
#define UHCI_TD_POOL_COUNT 256u

/* UHCI I/O register offsets */
#define UHCI_USBCMD   0x00
#define UHCI_USBSTS   0x02
#define UHCI_USBINTR  0x04
#define UHCI_FRNUM    0x06
#define UHCI_FLBASEAD 0x08
#define UHCI_SOFMOD   0x0C
#define UHCI_PORTSC1  0x10
#define UHCI_PORTSC2  0x12

#define UHCI_USBCMD_RS      0x0001u
#define UHCI_USBCMD_HCRESET 0x0002u
#define UHCI_USBCMD_CF      0x0040u
#define UHCI_USBCMD_MAXP    0x0080u

#define UHCI_USBSTS_USBINT   0x0001u
#define UHCI_USBSTS_ERROR    0x0002u
#define UHCI_USBSTS_HCHALTED 0x0020u

#define UHCI_PTR_T 0x00000001u
#define UHCI_PTR_QH 0x00000002u

#define UHCI_PORTSC_CCS   0x0001u
#define UHCI_PORTSC_CSC   0x0002u
#define UHCI_PORTSC_PE    0x0004u
#define UHCI_PORTSC_PEC   0x0008u
#define UHCI_PORTSC_LSDA  0x0100u
#define UHCI_PORTSC_PR    0x0200u

#define UHCI_TD_STATUS_ACTIVE 0x00800000u
#define UHCI_TD_STATUS_IOC    0x01000000u
#define UHCI_TD_STATUS_LS     0x04000000u
#define UHCI_TD_STATUS_CERR3  0x18000000u

#define USB_PID_OUT   0xE1u
#define USB_PID_IN    0x69u
#define USB_PID_SETUP 0x2Du

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t link_ptr;
    uint32_t elem_ptr;
} uhci_qh_hw_t;

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t link_ptr;
    uint32_t status;
    uint32_t token;
    uint32_t buffer;
} uhci_td_hw_t;

typedef struct __attribute__((packed)) {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_pkt_t;

typedef struct {
    int used;
    uint16_t qh_idx;
    uint16_t td_idx;
    usb_dma_block_t buf;
    uint8_t dev_addr;
    uint8_t ep_addr;
    uint8_t low_speed;
    uint8_t dtoggle;
    uint16_t max_packet;
    uint8_t interval;
    uint8_t _pad;
} uhci_intrq_t;

typedef struct {
    usb_dma_block_t frame_list;
    usb_dma_block_t qh_pool;
    usb_dma_block_t td_pool;
    uint16_t qh_free_head;
    uint16_t td_free_head;
    uint16_t qh_next[UHCI_QH_POOL_COUNT];
    uint16_t td_next[UHCI_TD_POOL_COUNT];
    uint16_t qh_schedule_head;
    uhci_intrq_t intrq[8];
} uhci_priv_t;

static uhci_priv_t g_uhci_priv[4];
static int g_uhci_priv_used[4];

static uhci_priv_t *uhci_priv_alloc_slot(void) {
    for (int i = 0; i < 4; ++i) {
        if (!g_uhci_priv_used[i]) {
            g_uhci_priv_used[i] = 1;
            memset(&g_uhci_priv[i], 0, sizeof(g_uhci_priv[i]));
            return &g_uhci_priv[i];
        }
    }
    return 0;
}

static uhci_priv_t *uhci_priv_from_uc(const uhci_controller_t *uc) {
    if (!uc || !uc->used || !uc->priv) return 0;
    return (uhci_priv_t *)uc->priv;
}

static uint32_t uhci_qh_phys(uhci_priv_t *p, uint16_t idx) {
    return p->qh_pool.paddr + (uint32_t)idx * sizeof(uhci_qh_hw_t);
}

static uhci_qh_hw_t *uhci_qh_virt(uhci_priv_t *p, uint16_t idx) {
    return (uhci_qh_hw_t *)((uint8_t *)p->qh_pool.vaddr + (uint32_t)idx * sizeof(uhci_qh_hw_t));
}

static uint32_t uhci_td_phys(uhci_priv_t *p, uint16_t idx) {
    return p->td_pool.paddr + (uint32_t)idx * sizeof(uhci_td_hw_t);
}

static uhci_td_hw_t *uhci_td_virt(uhci_priv_t *p, uint16_t idx) {
    return (uhci_td_hw_t *)((uint8_t *)p->td_pool.vaddr + (uint32_t)idx * sizeof(uhci_td_hw_t));
}

static int uhci_qh_alloc(uhci_priv_t *p, uint16_t *idx_out) {
    uint16_t idx;
    if (!p || !idx_out) return -1;
    idx = p->qh_free_head;
    if (idx == 0xFFFFu) return -1;
    p->qh_free_head = p->qh_next[idx];
    p->qh_next[idx] = 0xFFFFu;
    memset(uhci_qh_virt(p, idx), 0, sizeof(uhci_qh_hw_t));
    *idx_out = idx;
    return 0;
}

static int uhci_td_alloc(uhci_priv_t *p, uint16_t *idx_out) {
    uint16_t idx;
    if (!p || !idx_out) return -1;
    idx = p->td_free_head;
    if (idx == 0xFFFFu) return -1;
    p->td_free_head = p->td_next[idx];
    p->td_next[idx] = 0xFFFFu;
    memset(uhci_td_virt(p, idx), 0, sizeof(uhci_td_hw_t));
    *idx_out = idx;
    return 0;
}

static void uhci_frame_list_fill(uint32_t *fl, uint32_t ptr) {
    for (uint32_t i = 0; i < UHCI_FRAME_COUNT; ++i) fl[i] = ptr;
}

static uint16_t uhci_bar0_iobase(uint32_t bar0) {
    if ((bar0 & 1u) == 0) return 0; /* UHCI should use I/O BAR */
    return (uint16_t)(bar0 & ~0x1Fu);
}

static void uhci_write16(uint16_t base, uint16_t reg, uint16_t v) { outports((uint16_t)(base + reg), v); }
static uint16_t uhci_read16(uint16_t base, uint16_t reg) { return inports((uint16_t)(base + reg)); }
static void uhci_write32(uint16_t base, uint16_t reg, uint32_t v) { outportl((uint16_t)(base + reg), v); }

static void uhci_hw_reset(uint16_t base) {
    uhci_write16(base, UHCI_USBCMD, UHCI_USBCMD_HCRESET);
    for (volatile int i = 0; i < 200000; ++i) { (void)i; }
}

static void uhci_hw_start(uint16_t base, uint32_t frame_list_phys) {
    uhci_write16(base, UHCI_USBSTS, 0xFFFFu); /* ack pending */
    uhci_write16(base, UHCI_USBINTR, 0x0000u); /* polling mode for now */
    uhci_write16(base, UHCI_FRNUM, 0u);
    uhci_write32(base, UHCI_FLBASEAD, frame_list_phys);
    uhci_write16(base, UHCI_SOFMOD, 64u);
    uhci_write16(base, UHCI_USBCMD, UHCI_USBCMD_CF | UHCI_USBCMD_MAXP | UHCI_USBCMD_RS);
}

static void uhci_build_idle_schedule(uhci_priv_t *p) {
    uint16_t qh_idx, td_idx;
    uhci_qh_hw_t *qh;
    uhci_td_hw_t *td;
    uint32_t *fl;
    if (uhci_qh_alloc(p, &qh_idx) < 0) return;
    if (uhci_td_alloc(p, &td_idx) < 0) return;

    qh = uhci_qh_virt(p, qh_idx);
    td = uhci_td_virt(p, td_idx);

    /* Terminal queue head with one terminal TD keeps a valid schedule root. */
    td->link_ptr = UHCI_PTR_T;
    td->status = 0;
    td->token = 0;
    td->buffer = 0;

    qh->link_ptr = UHCI_PTR_T;
    qh->elem_ptr = uhci_td_phys(p, td_idx);

    p->qh_schedule_head = qh_idx;
    fl = (uint32_t *)p->frame_list.vaddr;
    uhci_frame_list_fill(fl, uhci_qh_phys(p, qh_idx) | UHCI_PTR_QH);
}

int uhci_init_controller(uhci_controller_t *uc,
                         uint8_t bus, uint8_t dev, uint8_t fn,
                         uint16_t vendor, uint16_t device,
                         uint32_t bar0, uint8_t irq_line) {
    uhci_priv_t *p;
    uint16_t iobase;
    if (!uc) return -1;
    memset(uc, 0, sizeof(*uc));
    iobase = uhci_bar0_iobase(bar0);
    if (iobase == 0) return -1;

    p = uhci_priv_alloc_slot();
    if (!p) return -1;

    if (usb_dma_alloc_zero(UHCI_FRAME_COUNT * sizeof(uint32_t), UHCI_FRAME_ALIGN, &p->frame_list) < 0) return -1;
    if (usb_dma_alloc_zero(UHCI_QH_POOL_COUNT * sizeof(uhci_qh_hw_t), UHCI_QH_ALIGN, &p->qh_pool) < 0) return -1;
    if (usb_dma_alloc_zero(UHCI_TD_POOL_COUNT * sizeof(uhci_td_hw_t), UHCI_TD_ALIGN, &p->td_pool) < 0) return -1;

    p->qh_free_head = 0;
    p->td_free_head = 0;
    for (uint16_t i = 0; i < UHCI_QH_POOL_COUNT; ++i) p->qh_next[i] = (i + 1u < UHCI_QH_POOL_COUNT) ? (uint16_t)(i + 1u) : 0xFFFFu;
    for (uint16_t i = 0; i < UHCI_TD_POOL_COUNT; ++i) p->td_next[i] = (i + 1u < UHCI_TD_POOL_COUNT) ? (uint16_t)(i + 1u) : 0xFFFFu;
    p->qh_schedule_head = 0xFFFFu;

    uhci_build_idle_schedule(p);

    uc->used = 1;
    uc->bus = bus;
    uc->dev = dev;
    uc->fn = fn;
    uc->irq_line = irq_line;
    uc->io_base = iobase;
    uc->vendor = vendor;
    uc->device = device;
    uc->frame_list_virt = p->frame_list.vaddr;
    uc->frame_list_phys = p->frame_list.paddr;
    uc->qh_pool_phys = p->qh_pool.paddr;
    uc->td_pool_phys = p->td_pool.paddr;
    uc->qh_count = UHCI_QH_POOL_COUNT;
    uc->td_count = UHCI_TD_POOL_COUNT;
    uc->priv = p;

    uhci_hw_reset(iobase);
    uhci_hw_start(iobase, uc->frame_list_phys);

    printf("[usb][uhci] %u:%u.%u io=0x%x frame=0x%x qh=0x%x td=0x%x\n",
           (uint32_t)bus, (uint32_t)dev, (uint32_t)fn,
           (uint32_t)iobase, uc->frame_list_phys, uc->qh_pool_phys, uc->td_pool_phys);
    return 0;
}

void uhci_poll_controller(uhci_controller_t *uc) {
    uint16_t st;
    if (!uc || !uc->used || uc->io_base == 0) return;
    st = uhci_read16(uc->io_base, UHCI_USBSTS);
    if (st && st != UHCI_USBSTS_HCHALTED) {
        uhci_write16(uc->io_base, UHCI_USBSTS, st);
    }
}

void uhci_debug_dump(const uhci_controller_t *uc) {
    if (!uc || !uc->used) return;
    printf("[usb][uhci] ctl %u:%u.%u io=0x%x irq=%u frame=0x%x qhN=%u tdN=%u\n",
           (uint32_t)uc->bus, (uint32_t)uc->dev, (uint32_t)uc->fn,
           (uint32_t)uc->io_base, (uint32_t)uc->irq_line,
           uc->frame_list_phys, (uint32_t)uc->qh_count, (uint32_t)uc->td_count);
}

uint32_t uhci_link_qh_ptr(uint32_t phys_qh) { return (phys_qh & ~0xFu) | UHCI_PTR_QH; }
uint32_t uhci_link_td_ptr(uint32_t phys_td) { return (phys_td & ~0xFu); }
uint32_t uhci_link_term_ptr(void) { return UHCI_PTR_T; }

int uhci_alloc_qh(uhci_controller_t *uc, uint16_t *idx_out, uint32_t *phys_out, void **virt_out) {
    uhci_priv_t *p = uhci_priv_from_uc(uc);
    uint16_t idx;
    if (!p) return -1;
    if (uhci_qh_alloc(p, &idx) < 0) return -1;
    if (idx_out) *idx_out = idx;
    if (phys_out) *phys_out = uhci_qh_phys(p, idx);
    if (virt_out) *virt_out = uhci_qh_virt(p, idx);
    return 0;
}

int uhci_alloc_td(uhci_controller_t *uc, uint16_t *idx_out, uint32_t *phys_out, void **virt_out) {
    uhci_priv_t *p = uhci_priv_from_uc(uc);
    uint16_t idx;
    if (!p) return -1;
    if (uhci_td_alloc(p, &idx) < 0) return -1;
    if (idx_out) *idx_out = idx;
    if (phys_out) *phys_out = uhci_td_phys(p, idx);
    if (virt_out) *virt_out = uhci_td_virt(p, idx);
    return 0;
}

int uhci_qh_set(uhci_controller_t *uc, uint16_t qh_idx, uint32_t link_ptr, uint32_t elem_ptr) {
    uhci_priv_t *p = uhci_priv_from_uc(uc);
    uhci_qh_hw_t *qh;
    if (!p || qh_idx >= UHCI_QH_POOL_COUNT) return -1;
    qh = uhci_qh_virt(p, qh_idx);
    qh->link_ptr = link_ptr;
    qh->elem_ptr = elem_ptr;
    return 0;
}

int uhci_td_set(uhci_controller_t *uc, uint16_t td_idx, uint32_t link_ptr, uint32_t status, uint32_t token, uint32_t buffer) {
    uhci_priv_t *p = uhci_priv_from_uc(uc);
    uhci_td_hw_t *td;
    if (!p || td_idx >= UHCI_TD_POOL_COUNT) return -1;
    td = uhci_td_virt(p, td_idx);
    td->link_ptr = link_ptr;
    td->status = status;
    td->token = token;
    td->buffer = buffer;
    return 0;
}

int uhci_append_async_qh(uhci_controller_t *uc, uint16_t qh_idx) {
    uhci_priv_t *p = uhci_priv_from_uc(uc);
    uint16_t prev_idx;
    uhci_qh_hw_t *prev, *newqh;
    if (!p || qh_idx >= UHCI_QH_POOL_COUNT || p->qh_schedule_head == 0xFFFFu) return -1;
    prev_idx = p->qh_schedule_head;
    while (1) {
        uint32_t lp;
        prev = uhci_qh_virt(p, prev_idx);
        lp = prev->link_ptr;
        if (lp & UHCI_PTR_T) break;
        if ((lp & UHCI_PTR_QH) == 0) break;
        prev_idx = (uint16_t)(((lp & ~0xFu) - p->qh_pool.paddr) / sizeof(uhci_qh_hw_t));
        if (prev_idx >= UHCI_QH_POOL_COUNT) return -1;
    }
    newqh = uhci_qh_virt(p, qh_idx);
    newqh->link_ptr = UHCI_PTR_T;
    prev->link_ptr = uhci_link_qh_ptr(uhci_qh_phys(p, qh_idx));
    return 0;
}

static uint16_t uhci_port_reg(int port_index) {
    return (port_index == 1) ? UHCI_PORTSC2 : UHCI_PORTSC1;
}

static void uhci_delay_spin(unsigned loops) {
    for (volatile unsigned i = 0; i < loops; ++i) { (void)i; }
}

static uint32_t uhci_td_token(uint8_t pid, uint8_t addr, uint8_t ep, uint8_t dtoggle, uint16_t len) {
    uint32_t maxlen = (len == 0) ? 0x7FFu : (uint32_t)(len - 1u);
    return ((uint32_t)pid) |
           ((uint32_t)(addr & 0x7Fu) << 8) |
           ((uint32_t)(ep & 0x0Fu) << 15) |
           ((uint32_t)(dtoggle & 1u) << 19) |
           (maxlen << 21);
}

static int uhci_wait_td_chain_done(uhci_controller_t *uc, const uint16_t *tds, int tdn, uint32_t timeout_loops) {
    uhci_priv_t *p = uhci_priv_from_uc(uc);
    if (!p || !tds || tdn <= 0) return -1;
    while (timeout_loops--) {
        int done = 1;
        for (int i = 0; i < tdn; ++i) {
            if (tds[i] >= UHCI_TD_POOL_COUNT) return -1;
            if (uhci_td_virt(p, tds[i])->status & UHCI_TD_STATUS_ACTIVE) {
                done = 0;
                break;
            }
        }
        if (done) return 0;
        uhci_poll_controller(uc);
        uhci_delay_spin(5000);
    }
    return -1;
}

static uint16_t uhci_td_actual_len(const uhci_td_hw_t *td) {
    uint32_t v;
    if (!td) return 0;
    v = td->status & 0x7FFu;
    return (v == 0x7FFu) ? 0u : (uint16_t)(v + 1u);
}

static int uhci_intrq_rearm(uhci_controller_t *uc, uhci_intrq_t *iq) {
    uhci_priv_t *p = uhci_priv_from_uc(uc);
    uhci_qh_hw_t *qh;
    uhci_td_hw_t *td;
    if (!p || !iq || !iq->used || iq->td_idx >= UHCI_TD_POOL_COUNT || iq->qh_idx >= UHCI_QH_POOL_COUNT) return -1;
    qh = uhci_qh_virt(p, iq->qh_idx);
    td = uhci_td_virt(p, iq->td_idx);
    td->link_ptr = uhci_link_term_ptr();
    td->status = UHCI_TD_STATUS_ACTIVE | UHCI_TD_STATUS_IOC | UHCI_TD_STATUS_CERR3 |
                 (iq->low_speed ? UHCI_TD_STATUS_LS : 0);
    td->token = uhci_td_token(USB_PID_IN, iq->dev_addr, (uint8_t)(iq->ep_addr & 0x0Fu), iq->dtoggle, iq->max_packet);
    td->buffer = iq->buf.paddr;
    /* HC advances QH->elem_ptr when a TD completes; restore it for the next poll cycle. */
    qh->elem_ptr = uhci_link_td_ptr(uhci_td_phys(p, iq->td_idx));
    return 0;
}

int uhci_port_connected(uhci_controller_t *uc, int port_index) {
    uint16_t reg, ps;
    if (!uc || !uc->used || (port_index != 0 && port_index != 1)) return 0;
    reg = uhci_port_reg(port_index);
    ps = uhci_read16(uc->io_base, reg);
    return (ps & UHCI_PORTSC_CCS) ? 1 : 0;
}

int uhci_port_reset_enable(uhci_controller_t *uc, int port_index, int *low_speed_out) {
    uint16_t reg, ps;
    if (!uc || !uc->used || (port_index != 0 && port_index != 1)) return -1;
    reg = uhci_port_reg(port_index);

    ps = uhci_read16(uc->io_base, reg);
    uhci_write16(uc->io_base, reg, (uint16_t)((ps | UHCI_PORTSC_PR) & ~(UHCI_PORTSC_CSC | UHCI_PORTSC_PEC)));
    uhci_delay_spin(3000000); /* >10ms rough spin */
    ps = uhci_read16(uc->io_base, reg);
    uhci_write16(uc->io_base, reg, (uint16_t)((ps & ~UHCI_PORTSC_PR) | UHCI_PORTSC_CSC | UHCI_PORTSC_PEC));
    uhci_delay_spin(1000000);

    ps = uhci_read16(uc->io_base, reg);
    if (!(ps & UHCI_PORTSC_CCS)) return -1;

    /* Ack change bits and try enabling port. */
    uhci_write16(uc->io_base, reg, (uint16_t)(UHCI_PORTSC_CSC | UHCI_PORTSC_PEC));
    ps = uhci_read16(uc->io_base, reg);
    uhci_write16(uc->io_base, reg, (uint16_t)((ps | UHCI_PORTSC_PE) | UHCI_PORTSC_CSC | UHCI_PORTSC_PEC));
    uhci_delay_spin(200000);
    ps = uhci_read16(uc->io_base, reg);
    if (low_speed_out) *low_speed_out = (ps & UHCI_PORTSC_LSDA) ? 1 : 0;
    return (ps & UHCI_PORTSC_PE) ? 0 : -1;
}

int uhci_control_transfer(uhci_controller_t *uc, int low_speed,
                          uint8_t dev_addr, uint8_t request_type, uint8_t request,
                          uint16_t value, uint16_t index,
                          void *data, uint16_t length, int in_dir) {
    uhci_priv_t *p = uhci_priv_from_uc(uc);
    usb_dma_block_t setup_dma, data_dma;
    usb_setup_pkt_t *sp;
    uint16_t qh_idx;
    uint32_t qh_phys;
    uhci_qh_hw_t *qh;
    uint16_t td_idx[32];
    int tdn = 0;
    uint8_t dt = 0;
    uint32_t status_base = UHCI_TD_STATUS_ACTIVE | UHCI_TD_STATUS_CERR3 | (low_speed ? UHCI_TD_STATUS_LS : 0);
    uint16_t done = 0;
    uint16_t remaining = length;
    uint8_t *bufp = (uint8_t *)data;
    int rc = -1;

    if (!p) return -1;
    if ((length > 0) && !data) return -1;
    if (length > 1024) return -1;
    if (uhci_alloc_qh(uc, &qh_idx, &qh_phys, (void **)&qh) < 0) return -1;
    if (usb_dma_alloc_zero(sizeof(usb_setup_pkt_t), 16, &setup_dma) < 0) return -1;
    if (length > 0) {
        if (usb_dma_alloc_zero(length, 16, &data_dma) < 0) return -1;
        if (!in_dir) memcpy(data_dma.vaddr, data, length);
    } else {
        memset(&data_dma, 0, sizeof(data_dma));
    }

    sp = (usb_setup_pkt_t *)setup_dma.vaddr;
    sp->bmRequestType = request_type;
    sp->bRequest = request;
    sp->wValue = value;
    sp->wIndex = index;
    sp->wLength = length;

    /* Setup TD */
    if (uhci_alloc_td(uc, &td_idx[tdn], 0, 0) < 0) return -1;
    uhci_td_set(uc, td_idx[tdn], uhci_link_term_ptr(),
                status_base,
                uhci_td_token(USB_PID_SETUP, dev_addr, 0, 0, sizeof(usb_setup_pkt_t)),
                setup_dma.paddr);
    tdn++;
    dt = 1;

    while (remaining > 0) {
        uint16_t chunk = (remaining > 8u) ? 8u : remaining;
        uint8_t pid = in_dir ? USB_PID_IN : USB_PID_OUT;
        if (tdn >= (int)(sizeof(td_idx) / sizeof(td_idx[0])) - 1) return -1;
        if (uhci_alloc_td(uc, &td_idx[tdn], 0, 0) < 0) return -1;
        uhci_td_set(uc, td_idx[tdn], uhci_link_term_ptr(),
                    status_base,
                    uhci_td_token(pid, dev_addr, 0, dt, chunk),
                    data_dma.paddr + done);
        tdn++;
        done += chunk;
        remaining -= chunk;
        dt ^= 1u;
    }

    /* Status TD: opposite direction, DATA1 */
    if (uhci_alloc_td(uc, &td_idx[tdn], 0, 0) < 0) return -1;
    uhci_td_set(uc, td_idx[tdn], uhci_link_term_ptr(),
                status_base | UHCI_TD_STATUS_IOC,
                uhci_td_token(in_dir ? USB_PID_OUT : USB_PID_IN, dev_addr, 0, 1, 0),
                0);
    tdn++;

    for (int i = 0; i < tdn - 1; ++i) {
        uhci_td_hw_t *td = uhci_td_virt(p, td_idx[i]);
        td->link_ptr = uhci_link_td_ptr(uhci_td_phys(p, td_idx[i + 1]));
    }

    qh->elem_ptr = uhci_link_td_ptr(uhci_td_phys(p, td_idx[0]));
    qh->link_ptr = uhci_link_term_ptr();
    if (uhci_append_async_qh(uc, qh_idx) < 0) return -1;

    rc = uhci_wait_td_chain_done(uc, td_idx, tdn, 4000);
    if (rc < 0) return -1;

    for (int i = 0; i < tdn; ++i) {
        uhci_td_hw_t *td = uhci_td_virt(p, td_idx[i]);
        if (td->status & UHCI_TD_STATUS_ACTIVE) return -1;
        if (td->status & 0x007F0000u) return -1; /* error bits */
    }

    if (in_dir && length > 0) memcpy(bufp, data_dma.vaddr, length);
    return 0;
}

int uhci_intr_queue_open(uhci_controller_t *uc, int low_speed,
                         uint8_t dev_addr, uint8_t ep_addr,
                         uint16_t max_packet, uint8_t interval,
                         uhci_intr_queue_t *out_q) {
    uhci_priv_t *p = uhci_priv_from_uc(uc);
    int slot = -1;
    uhci_intrq_t *iq;
    uhci_qh_hw_t *qh;
    uint16_t qh_idx, td_idx;
    uint32_t qh_phys;
    if (!p || !out_q || max_packet == 0 || max_packet > 64) return -1;
    for (int i = 0; i < (int)(sizeof(p->intrq)/sizeof(p->intrq[0])); ++i) {
        if (!p->intrq[i].used) { slot = i; break; }
    }
    if (slot < 0) return -1;
    if (uhci_alloc_qh(uc, &qh_idx, &qh_phys, (void **)&qh) < 0) return -1;
    if (uhci_alloc_td(uc, &td_idx, 0, 0) < 0) return -1;

    iq = &p->intrq[slot];
    memset(iq, 0, sizeof(*iq));
    if (usb_dma_alloc_zero(max_packet, 16, &iq->buf) < 0) return -1;
    iq->used = 1;
    iq->qh_idx = qh_idx;
    iq->td_idx = td_idx;
    iq->dev_addr = dev_addr;
    iq->ep_addr = ep_addr;
    iq->low_speed = low_speed ? 1 : 0;
    iq->dtoggle = 0;
    iq->max_packet = max_packet;
    iq->interval = interval ? interval : 10;

    if (uhci_intrq_rearm(uc, iq) < 0) return -1;
    qh->elem_ptr = uhci_link_td_ptr(uhci_td_phys(p, td_idx));
    qh->link_ptr = uhci_link_term_ptr();
    if (uhci_append_async_qh(uc, qh_idx) < 0) return -1;

    memset(out_q, 0, sizeof(*out_q));
    out_q->priv = p;
    out_q->slot = (uint8_t)slot;
    return 0;
}

int uhci_intr_queue_poll(uhci_controller_t *uc, uhci_intr_queue_t *q,
                         void *out_buf, uint16_t out_max, uint16_t *out_len) {
    uhci_priv_t *p = uhci_priv_from_uc(uc);
    uhci_intrq_t *iq;
    uhci_td_hw_t *td;
    uint16_t got;
    if (out_len) *out_len = 0;
    if (!p || !q || q->priv != p) return -1;
    if (q->slot >= (uint8_t)(sizeof(p->intrq)/sizeof(p->intrq[0]))) return -1;
    iq = &p->intrq[q->slot];
    if (!iq->used) return -1;
    td = uhci_td_virt(p, iq->td_idx);
    if (td->status & UHCI_TD_STATUS_ACTIVE) return 0;
    if (td->status & 0x007F0000u) {
        (void)uhci_intrq_rearm(uc, iq);
        return -1;
    }
    got = uhci_td_actual_len(td);
    if (got > iq->max_packet) got = iq->max_packet;
    if (out_buf && out_max && got) {
        if (got > out_max) got = out_max;
        memcpy(out_buf, iq->buf.vaddr, got);
    }
    if (out_len) *out_len = got;
    iq->dtoggle ^= 1u;
    (void)uhci_intrq_rearm(uc, iq);
    return got ? 1 : 0;
}
