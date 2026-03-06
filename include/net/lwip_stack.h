#ifndef NET_LWIP_STACK_H
#define NET_LWIP_STACK_H

#include <stdint.h>

void lwip_stack_init(void);
void lwip_stack_poll(void);
int lwip_stack_is_ready(void);
int lwip_stack_send_icmp_echo(uint32_t dst_ip_be, const uint8_t *icmp_payload, uint16_t icmp_len, uint8_t ttl);
int lwip_stack_recv_icmp_reply_for_id(uint16_t id_be, uint8_t *ip_packet_out, uint32_t *ip_packet_len, uint32_t *src_ip_be);
int lwip_stack_recv_icmp_packet(uint8_t *ip_packet_out, uint32_t *ip_packet_len, uint32_t *src_ip_be);
int lwip_stack_send_icmpv6_echo(const uint8_t dst_ip6[16], const uint8_t *icmp_payload, uint16_t icmp_len, uint8_t hop_limit);
int lwip_stack_recv_icmpv6_reply_for_id(uint16_t id_be, uint8_t *packet_out, uint32_t *packet_len, uint8_t src_ip6_out[16]);
int lwip_stack_get_ipv6_addr(uint8_t out[16], int prefer_global);
int lwip_stack_get_ipv6_addr_at(int ordinal, uint8_t out[16], uint8_t *prefix_len, uint8_t *scope, uint8_t *flags);
void lwip_stack_get_link_stats(uint64_t *rx_packets, uint64_t *rx_bytes, uint64_t *tx_packets, uint64_t *tx_bytes);
int lwip_stack_reload_system_config(void);
int lwip_stack_set_hostname(const char *name);
const char *lwip_stack_get_hostname(void);

#endif
