#ifndef DRIVERS_XHCI_H
#define DRIVERS_XHCI_H

#include <stdint.h>
#include "drivers/usb_dma.h"

#define XHCI_MAX_TRACKED_SLOTS 32u

typedef struct {
    uint8_t used;
    uint8_t online;
    uint8_t slot_id;
    uint8_t port_id;
    uint8_t speed_id;
    uint8_t hid_ready;
    uint8_t hid_iface;
    uint8_t hid_ep_addr;
    uint8_t hid_ep_dci;
    uint8_t hid_interval;
    uint16_t hid_max_packet;
    uint16_t max_packet0;
    uint16_t _pad;
    uint8_t ep0_ccs;
    uint8_t ep0_enq;
    uint8_t intr_ccs;
    uint8_t intr_enq;
    usb_dma_block_t input_ctx;
    usb_dma_block_t device_ctx;
    usb_dma_block_t ep0_ring;
    usb_dma_block_t ctrl_buf;
    usb_dma_block_t intr_ring;
    usb_dma_block_t intr_buf;
    uint64_t intr_pending_trb;
} xhci_slot_state_t;

typedef struct {
    int used;
    uint8_t bus, dev, fn;
    uint8_t irq_line;
    uint16_t vendor, device;
    uint64_t mmio_base;
    uint8_t cap_len;
    uint8_t max_ports;
    uint8_t ext_cap_off;
    volatile uint8_t *mmio;
    volatile uint8_t *op;
    volatile uint8_t *rt;
    volatile uint8_t *db;
    uint8_t max_slots;
    uint8_t cmd_ccs;
    uint8_t evt_ccs;
    uint8_t ctx_sz64;
    uint8_t cmd_enq;
    uint32_t cmd_ring_size;
    uint32_t evt_ring_size;
    uint32_t evt_deq;
    uint64_t cmd_wait_ptr;
    uint8_t cmd_wait_done;
    uint8_t cmd_wait_cc;
    uint8_t cmd_wait_slot;
    uint8_t cmd_wait_ep;
    uint64_t xfer_wait_ptr;
    uint8_t xfer_wait_done;
    uint8_t xfer_wait_cc;
    uint8_t xfer_wait_slot;
    uint8_t xfer_wait_ep;
    usb_dma_block_t dcbaa;
    usb_dma_block_t cmd_ring;
    usb_dma_block_t evt_ring;
    usb_dma_block_t erst;
    uint8_t port_to_slot[256];
    xhci_slot_state_t slots[XHCI_MAX_TRACKED_SLOTS + 1u];
    int running;
} xhci_controller_t;

int xhci_init_controller(xhci_controller_t *xc,
                         uint8_t bus, uint8_t dev, uint8_t fn,
                         uint16_t vendor, uint16_t device,
                         uint32_t bar0, uint32_t bar1, uint8_t irq_line);
void xhci_poll_controller(xhci_controller_t *xc);
void xhci_debug_dump(const xhci_controller_t *xc);

#endif
