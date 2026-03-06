#ifndef DRIVERS_USB_DMA_H
#define DRIVERS_USB_DMA_H

#include <stdint.h>

typedef struct {
    void *vaddr;
    uint32_t paddr;
    uint32_t size;
} usb_dma_block_t;

void usb_dma_init(void);
int usb_dma_alloc(uint32_t size, uint32_t align, usb_dma_block_t *out);
int usb_dma_alloc_zero(uint32_t size, uint32_t align, usb_dma_block_t *out);
uint32_t usb_dma_bytes_total(void);
uint32_t usb_dma_bytes_used(void);

#endif
