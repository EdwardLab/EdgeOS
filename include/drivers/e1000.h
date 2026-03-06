#ifndef DRIVERS_E1000_H
#define DRIVERS_E1000_H

#include <stdint.h>

typedef void (*e1000_rx_frame_cb_t)(const uint8_t *frame, uint32_t len);

void e1000_init(void);
int e1000_is_ready(void);
void e1000_poll(void);
int e1000_get_mac(uint8_t mac_out[6]);
int e1000_send_frame_raw(const void *frame, uint16_t len);
void e1000_set_rx_frame_callback(e1000_rx_frame_cb_t cb);
int e1000_send_icmp_echo(uint32_t dst_ip_be, const uint8_t *icmp_payload, uint16_t icmp_len);
int e1000_recv_icmp_reply_for_id(uint16_t id_be, uint8_t *ip_packet_out, uint32_t *ip_packet_len, uint32_t *src_ip_be);

#endif
