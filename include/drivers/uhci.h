#ifndef DRIVERS_UHCI_H
#define DRIVERS_UHCI_H

#include <stdint.h>

typedef struct {
    int used;
    uint8_t bus, dev, fn;
    uint8_t irq_line;
    uint16_t io_base;
    uint16_t vendor, device;
    uint32_t frame_list_phys;
    void *frame_list_virt;
    uint32_t td_pool_phys;
    uint32_t qh_pool_phys;
    uint16_t td_count;
    uint16_t qh_count;
    void *priv;
} uhci_controller_t;

typedef struct {
    void *priv;
    uint8_t slot;
    uint8_t _pad[3];
} uhci_intr_queue_t;

int uhci_init_controller(uhci_controller_t *uc,
                         uint8_t bus, uint8_t dev, uint8_t fn,
                         uint16_t vendor, uint16_t device,
                         uint32_t bar0, uint8_t irq_line);
void uhci_poll_controller(uhci_controller_t *uc);
void uhci_debug_dump(const uhci_controller_t *uc);

int uhci_alloc_qh(uhci_controller_t *uc, uint16_t *idx_out, uint32_t *phys_out, void **virt_out);
int uhci_alloc_td(uhci_controller_t *uc, uint16_t *idx_out, uint32_t *phys_out, void **virt_out);
uint32_t uhci_link_qh_ptr(uint32_t phys_qh);
uint32_t uhci_link_td_ptr(uint32_t phys_td);
uint32_t uhci_link_term_ptr(void);
int uhci_qh_set(uhci_controller_t *uc, uint16_t qh_idx, uint32_t link_ptr, uint32_t elem_ptr);
int uhci_td_set(uhci_controller_t *uc, uint16_t td_idx, uint32_t link_ptr, uint32_t status, uint32_t token, uint32_t buffer);
int uhci_append_async_qh(uhci_controller_t *uc, uint16_t qh_idx);
int uhci_port_connected(uhci_controller_t *uc, int port_index);
int uhci_port_reset_enable(uhci_controller_t *uc, int port_index, int *low_speed_out);
int uhci_control_transfer(uhci_controller_t *uc, int low_speed,
                          uint8_t dev_addr, uint8_t request_type, uint8_t request,
                          uint16_t value, uint16_t index,
                          void *data, uint16_t length, int in_dir);
int uhci_intr_queue_open(uhci_controller_t *uc, int low_speed,
                         uint8_t dev_addr, uint8_t ep_addr,
                         uint16_t max_packet, uint8_t interval,
                         uhci_intr_queue_t *out_q);
int uhci_intr_queue_poll(uhci_controller_t *uc, uhci_intr_queue_t *q,
                         void *out_buf, uint16_t out_max, uint16_t *out_len);

#endif
