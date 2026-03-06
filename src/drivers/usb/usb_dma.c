#include "drivers/usb_dma.h"

#include "string.h"
#include "stdio.h"

#define USB_DMA_POOL_SIZE (512u * 1024u)

/* Current kernel maps low memory directly enough that device-visible physical
 * addresses for static kernel memory are obtained by casting pointers. This is
 * the same assumption used by existing AHCI code paths.
 */
static uint8_t g_usb_dma_pool[USB_DMA_POOL_SIZE] __attribute__((aligned(4096)));
static uint32_t g_usb_dma_off;

static uint32_t align_up32(uint32_t v, uint32_t a) {
    if (a == 0) return v;
    return (v + (a - 1u)) & ~(a - 1u);
}

void usb_dma_init(void) {
    g_usb_dma_off = 0;
    memset(g_usb_dma_pool, 0, sizeof(g_usb_dma_pool));
    printf("[usb][dma] pool %u KiB at %p\n", (uint32_t)(USB_DMA_POOL_SIZE / 1024u), g_usb_dma_pool);
}

int usb_dma_alloc(uint32_t size, uint32_t align, usb_dma_block_t *out) {
    uint32_t off, next;
    if (!out || size == 0) return -1;
    if (align == 0) align = 16;
    if ((align & (align - 1u)) != 0) return -1;
    off = align_up32(g_usb_dma_off, align);
    next = off + size;
    if (next < off || next > USB_DMA_POOL_SIZE) return -1;
    out->vaddr = &g_usb_dma_pool[off];
    out->paddr = (uint32_t)(uintptr_t)out->vaddr;
    out->size = size;
    g_usb_dma_off = next;
    return 0;
}

int usb_dma_alloc_zero(uint32_t size, uint32_t align, usb_dma_block_t *out) {
    if (usb_dma_alloc(size, align, out) < 0) return -1;
    memset(out->vaddr, 0, size);
    return 0;
}

uint32_t usb_dma_bytes_total(void) { return USB_DMA_POOL_SIZE; }
uint32_t usb_dma_bytes_used(void) { return g_usb_dma_off; }
