#include "net/lwip_stack.h"

#include "drivers/e1000.h"
#include "sys/boottime.h"
#include "string.h"
#include "stdio.h"
#include "vfs/vfs.h"

#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "lwip/netif.h"
#include "lwip/raw.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/ip6_addr.h"
#include "lwip/etharp.h"
#include "lwip/ethip6.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/icmp6.h"
#include "lwip/prot/ip6.h"
#include "lwip/dns.h"
#include "netif/ethernet.h"

#include <stdint.h>

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
} edge_ipv4_hdr_t;

typedef struct {
    uint8_t used;
    uint16_t id_be;
    uint32_t src_ip_be;
    uint32_t ip_len;
    uint8_t ip_pkt[1600];
} edge_icmp_reply_t;

typedef struct {
    uint8_t used;
    uint16_t id_be;
    uint8_t src_ip6[16];
    uint32_t pkt_len;
    uint8_t pkt[1600];
} edge_icmp6_reply_t;

#define EDGE_ICMP_REPLY_Q 16
#define EDGE_ICMP6_REPLY_Q 16
#define EDGE_ICMP_RAW_Q 32

static struct netif g_lwip_netif;
static struct raw_pcb *g_icmp_raw;
static struct raw_pcb *g_icmp6_raw;
static int g_ready;
static uint64_t g_rx_packets;
static uint64_t g_rx_bytes;
static uint64_t g_tx_packets;
static uint64_t g_tx_bytes;
static edge_icmp_reply_t g_reply_q[EDGE_ICMP_REPLY_Q];
static edge_icmp_reply_t g_raw_icmp_q[EDGE_ICMP_RAW_Q];
static edge_icmp6_reply_t g_reply6_q[EDGE_ICMP6_REPLY_Q];
static char g_hostname[65] = "edgeos";

static int edge_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int edge_valid_hostname_char(char c) {
    return ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_');
}

static int edge_read_text_file(const char *path, char *buf, int buf_sz) {
    int n;
    if (!path || !buf || buf_sz <= 1) return -1;
    n = vfs_read_file(path, buf, (uint32_t)(buf_sz - 1));
    if (n < 0) return -1;
    if (n >= buf_sz) n = buf_sz - 1;
    buf[n] = 0;
    return n;
}

static int edge_parse_hostname_token(const char *src, char *out, int out_sz) {
    int si = 0;
    int oi = 0;
    if (!src || !out || out_sz <= 1) return -1;
    while (src[si] && edge_is_space(src[si])) si++;
    while (src[si] && !edge_is_space(src[si]) && src[si] != '#') {
        char c = src[si++];
        if (!edge_valid_hostname_char(c)) continue;
        if (oi >= out_sz - 1) return -1;
        out[oi++] = c;
    }
    out[oi] = 0;
    return oi > 0 ? 0 : -1;
}

static void edge_apply_dns_servers_from_text(const char *text) {
#if LWIP_DNS
    ip_addr_t parsed[DNS_MAX_SERVERS];
    int parsed_count = 0;
    int i = 0;
    char line[160];
    int line_len = 0;

    memset(parsed, 0, sizeof(parsed));
    while (text && text[i]) {
        char c = text[i++];
        if (c == '\r') continue;
        if (c == '\n' || line_len >= (int)sizeof(line) - 1) {
            int p = 0;
            char token[80];
            int ti = 0;
            line[line_len] = 0;
            line_len = 0;

            while (line[p] && edge_is_space(line[p])) p++;
            if (line[p] == '#') continue;
            if (strncmp(&line[p], "nameserver", 10) != 0) continue;
            if (line[p + 10] && !edge_is_space(line[p + 10])) continue;
            p += 10;
            while (line[p] && edge_is_space(line[p])) p++;
            while (line[p] && !edge_is_space(line[p]) && line[p] != '#' && ti < (int)sizeof(token) - 1) {
                token[ti++] = line[p++];
            }
            token[ti] = 0;
            if (ti == 0) continue;
            if (parsed_count >= DNS_MAX_SERVERS) continue;
            if (ipaddr_aton(token, &parsed[parsed_count])) parsed_count++;
            continue;
        }
        line[line_len++] = c;
    }
    if (line_len > 0) {
        int p = 0;
        char token[80];
        int ti = 0;
        line[line_len] = 0;
        while (line[p] && edge_is_space(line[p])) p++;
        if (line[p] != '#' &&
            strncmp(&line[p], "nameserver", 10) == 0 &&
            (!line[p + 10] || edge_is_space(line[p + 10]))) {
            p += 10;
            while (line[p] && edge_is_space(line[p])) p++;
            while (line[p] && !edge_is_space(line[p]) && line[p] != '#' && ti < (int)sizeof(token) - 1) {
                token[ti++] = line[p++];
            }
            token[ti] = 0;
            if (ti > 0 && parsed_count < DNS_MAX_SERVERS && ipaddr_aton(token, &parsed[parsed_count])) {
                parsed_count++;
            }
        }
    }

    for (i = 0; i < DNS_MAX_SERVERS; ++i) {
        ip_addr_t zero_ip;
        ip_addr_set_zero(&zero_ip);
        dns_setserver((u8_t)i, i < parsed_count ? &parsed[i] : &zero_ip);
    }
#else
    (void)text;
#endif
}

static void edge_try_reload_dns_from_resolv_conf(void) {
    char buf[2048];
    int n = edge_read_text_file("/etc/resolv.conf", buf, sizeof(buf));
    if (n < 0) return;
    edge_apply_dns_servers_from_text(buf);
}

static void edge_try_reload_hostname_from_file(void) {
    char buf[192];
    char parsed[65];
    if (edge_read_text_file("/etc/hostname", buf, sizeof(buf)) < 0) return;
    if (edge_parse_hostname_token(buf, parsed, sizeof(parsed)) < 0) return;
    (void)lwip_stack_set_hostname(parsed);
}

static void edge_ip6_to_bytes(const ip6_addr_t *a, uint8_t out[16]) {
    if (!a || !out) return;
    for (int i = 0; i < 4; ++i) {
        uint32_t w = lwip_htonl(a->addr[i]);
        out[i * 4 + 0] = (uint8_t)((w >> 24) & 0xFFu);
        out[i * 4 + 1] = (uint8_t)((w >> 16) & 0xFFu);
        out[i * 4 + 2] = (uint8_t)((w >> 8) & 0xFFu);
        out[i * 4 + 3] = (uint8_t)(w & 0xFFu);
    }
}

static int edge_extract_icmp_probe_id(const uint8_t *icmp, uint16_t icmp_len, uint16_t *id_be_out) {
    uint8_t type;
    if (!icmp || icmp_len < 8 || !id_be_out) return 0;
    type = icmp[0];
    if (type == ICMP_ER) {
        memcpy(id_be_out, &icmp[4], sizeof(*id_be_out));
        return 1;
    }
    if (type == 3 || type == 11) {
        const uint8_t *inner_ip = icmp + 8;
        uint16_t inner_len = (uint16_t)(icmp_len - 8);
        uint16_t ihl;
        const uint8_t *inner_icmp;
        if (inner_len < 20) return 0;
        if ((inner_ip[0] >> 4) != 4) return 0;
        ihl = (uint16_t)((inner_ip[0] & 0x0Fu) * 4u);
        if (ihl < 20 || ihl + 8 > inner_len) return 0;
        if (inner_ip[9] != 1) return 0;
        inner_icmp = inner_ip + ihl;
        memcpy(id_be_out, &inner_icmp[4], sizeof(*id_be_out));
        return 1;
    }
    return 0;
}

static int edge_extract_icmp6_probe_id(const uint8_t *icmp6, uint16_t icmp6_len, uint16_t *id_be_out) {
    if (!icmp6 || icmp6_len < 8 || !id_be_out) return 0;

    if (icmp6[0] == ICMP6_TYPE_EREP) {
        memcpy(id_be_out, &icmp6[4], sizeof(*id_be_out));
        return 1;
    }

    if (icmp6[0] == ICMP6_TYPE_DUR || icmp6[0] == ICMP6_TYPE_TE || icmp6[0] == ICMP6_TYPE_PP) {
        const uint8_t *inner_ip6 = icmp6 + 8;
        uint16_t inner_len = (uint16_t)(icmp6_len - 8);
        uint8_t nexth;
        if (inner_len < 48) return 0;
        if ((inner_ip6[0] >> 4) != 6) return 0;
        nexth = inner_ip6[6];
        if (nexth != IP6_NEXTH_ICMP6) return 0;
        memcpy(id_be_out, inner_ip6 + 40 + 4, sizeof(*id_be_out));
        return 1;
    }

    return 0;
}

static uint16_t edge_cksum16(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t acc = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2) acc += ((uint32_t)p[i] << 8) | p[i + 1];
    if (len & 1u) acc += (uint32_t)p[len - 1] << 8;
    while (acc >> 16) acc = (acc & 0xFFFFu) + (acc >> 16);
    return (uint16_t)~acc;
}

u32_t sys_now(void) {
    return (u32_t)(boottime_monotonic_us() / 1000ull);
}

static err_t edge_lwip_linkoutput(struct netif *netif, struct pbuf *p) {
    uint8_t frame[1600];
    uint16_t total;
    (void)netif;
    if (!p) return ERR_ARG;
    if (p->tot_len > sizeof(frame)) return ERR_MEM;
    if (pbuf_copy_partial(p, frame, p->tot_len, 0) != p->tot_len) return ERR_VAL;
    total = (uint16_t)p->tot_len;
    if (e1000_send_frame_raw(frame, total) < 0) return ERR_IF;
    g_tx_packets++;
    g_tx_bytes += total;
    return ERR_OK;
}

static err_t edge_lwip_netif_init(struct netif *netif) {
    uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    if (!netif) return ERR_ARG;
    netif->name[0] = 'e';
    netif->name[1] = '0';
    netif->output = etharp_output;
#if LWIP_IPV6
    netif->output_ip6 = ethip6_output;
#endif
    netif->linkoutput = edge_lwip_linkoutput;
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
#if LWIP_IPV6_MLD
    netif->flags |= NETIF_FLAG_MLD6;
#endif
#if LWIP_NETIF_HOSTNAME
    netif_set_hostname(netif, g_hostname);
#endif
    netif->hwaddr_len = 6;
    if (e1000_get_mac(mac) < 0) {
        printf("[net] lwip: warning using fallback MAC\n");
    }
    memcpy(netif->hwaddr, mac, 6);
    return ERR_OK;
}

static void edge_queue_ipv4_icmp_packet(edge_icmp_reply_t *queue, int qlen,
                                        const uint8_t *icmp, uint16_t icmp_len,
                                        uint32_t src_ip_be, uint16_t id_be) {
    edge_icmp_reply_t *slot = 0;
    edge_ipv4_hdr_t *ip;
    if (!queue || qlen <= 0 || !icmp || icmp_len < 8) return;

    for (int i = 0; i < qlen; ++i) {
        if (!queue[i].used) {
            slot = &queue[i];
            break;
        }
    }
    if (!slot) slot = &queue[0];

    memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    slot->id_be = id_be;
    slot->src_ip_be = src_ip_be;
    slot->ip_len = (uint32_t)(sizeof(edge_ipv4_hdr_t) + icmp_len);
    if (slot->ip_len > sizeof(slot->ip_pkt)) {
        slot->used = 0;
        return;
    }

    ip = (edge_ipv4_hdr_t *)slot->ip_pkt;
    memset(ip, 0, sizeof(*ip));
    ip->ver_ihl = 0x45;
    ip->ttl = 64;
    ip->proto = 1;
    ip->total_len_be = (uint16_t)(((slot->ip_len & 0x00FFu) << 8) | ((slot->ip_len & 0xFF00u) >> 8));
    ip->src_be = src_ip_be;
    ip->dst_be = 0x0F02000Au;
    ip->csum_be = edge_cksum16(ip, sizeof(*ip));
    memcpy(slot->ip_pkt + sizeof(*ip), icmp, icmp_len);
}

static void edge_queue_icmp_reply(const uint8_t *icmp, uint16_t icmp_len, uint32_t src_ip_be) {
    uint16_t id_be = 0;
    if (!icmp || icmp_len < 8) return;

    edge_queue_ipv4_icmp_packet(g_raw_icmp_q, EDGE_ICMP_RAW_Q, icmp, icmp_len, src_ip_be, 0);
    if (!edge_extract_icmp_probe_id(icmp, icmp_len, &id_be)) return;
    edge_queue_ipv4_icmp_packet(g_reply_q, EDGE_ICMP_REPLY_Q, icmp, icmp_len, src_ip_be, id_be);
}

static void edge_queue_icmp6_reply(const uint8_t *icmp6, uint16_t icmp6_len, const ip_addr_t *src) {
    edge_icmp6_reply_t *slot = 0;
    uint16_t id_be;
    if (!icmp6 || icmp6_len < 8 || !src || !IP_IS_V6(src)) return;
    if (!edge_extract_icmp6_probe_id(icmp6, icmp6_len, &id_be)) return;

    for (int i = 0; i < EDGE_ICMP6_REPLY_Q; ++i) {
        if (!g_reply6_q[i].used) {
            slot = &g_reply6_q[i];
            break;
        }
    }
    if (!slot) slot = &g_reply6_q[0];

    memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    slot->id_be = id_be;
    slot->pkt_len = icmp6_len;
    if (slot->pkt_len > sizeof(slot->pkt)) {
        slot->used = 0;
        return;
    }

    edge_ip6_to_bytes(ip_2_ip6(src), slot->src_ip6);
    memcpy(slot->pkt, icmp6, icmp6_len);
}

static u8_t edge_lwip_icmp_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr) {
    uint8_t buf[1536];
    const uint8_t *icmp = 0;
    uint16_t icmp_len = 0;
    uint16_t n;
    (void)arg;
    (void)pcb;
    if (!p || !addr || !IP_IS_V4(addr)) return 0;
    n = p->tot_len > sizeof(buf) ? (uint16_t)sizeof(buf) : (uint16_t)p->tot_len;
    if (n < 8) return 0;
    if (pbuf_copy_partial(p, buf, n, 0) != n) return 0;

    if ((buf[0] >> 4) == 4 && n >= 20) {
        uint16_t ihl = (uint16_t)((buf[0] & 0x0Fu) * 4u);
        if (ihl >= 20 && ihl + 8 <= n) {
            icmp = &buf[ihl];
            icmp_len = (uint16_t)(n - ihl);
        }
    } else {
        icmp = buf;
        icmp_len = n;
    }
    if (!icmp || icmp_len < 8) return 0;
    edge_queue_icmp_reply(icmp, icmp_len, ip4_addr_get_u32(ip_2_ip4(addr)));
    return 0;
}

static u8_t edge_lwip_icmp6_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr) {
    uint8_t buf[1600];
    const uint8_t *icmp6 = 0;
    uint16_t icmp6_len = 0;
    uint16_t n;
    (void)arg;
    (void)pcb;
    if (!p || !addr || !IP_IS_V6(addr)) return 0;
    n = p->tot_len > sizeof(buf) ? (uint16_t)sizeof(buf) : (uint16_t)p->tot_len;
    if (n < 8) return 0;
    if (pbuf_copy_partial(p, buf, n, 0) != n) return 0;

    if ((buf[0] >> 4) == 6 && n >= 48) {
        uint8_t nh = buf[6];
        if (nh == IP6_NEXTH_ICMP6) {
            icmp6 = &buf[40];
            icmp6_len = (uint16_t)(n - 40);
        }
    } else {
        icmp6 = buf;
        icmp6_len = n;
    }

    if (!icmp6 || icmp6_len < 8) return 0;
    edge_queue_icmp6_reply(icmp6, icmp6_len, addr);
    return 0;
}

static void edge_lwip_rx_frame_cb(const uint8_t *frame, uint32_t len) {
    struct pbuf *p;
    err_t err;
    if (!g_ready || !frame || len == 0 || len > 1600) return;
    g_rx_packets++;
    g_rx_bytes += len;

    p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    if (!p) return;
    if (pbuf_take(p, frame, (u16_t)len) != ERR_OK) {
        pbuf_free(p);
        return;
    }
    err = g_lwip_netif.input(p, &g_lwip_netif);
    if (err != ERR_OK) {
        printf("[net] lwip: netif->input err=%d\n", (int)err);
        pbuf_free(p);
    }
}

void lwip_stack_init(void) {
    ip4_addr_t ipaddr;
    ip4_addr_t netmask;
    ip4_addr_t gw;

    if (g_ready || !e1000_is_ready()) return;
    g_rx_packets = 0;
    g_rx_bytes = 0;
    g_tx_packets = 0;
    g_tx_bytes = 0;

    lwip_init();
    IP4_ADDR(&ipaddr, 10, 0, 2, 15);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 10, 0, 2, 2);

    if (!netif_add(&g_lwip_netif, &ipaddr, &netmask, &gw, 0, edge_lwip_netif_init, ethernet_input)) {
        printf("[net] lwip: netif_add failed\n");
        return;
    }

#if LWIP_IPV6
#if LWIP_IPV6_AUTOCONFIG
    g_lwip_netif.ip6_autoconfig_enabled = 1;
#endif
    netif_create_ip6_linklocal_address(&g_lwip_netif, 1);
#endif

    netif_set_default(&g_lwip_netif);
    netif_set_up(&g_lwip_netif);
    netif_set_link_up(&g_lwip_netif);

    g_icmp_raw = raw_new(IP_PROTO_ICMP);
    if (!g_icmp_raw) {
        printf("[net] lwip: raw_new(ICMP) failed\n");
        return;
    }
    raw_recv(g_icmp_raw, edge_lwip_icmp_recv, 0);
    raw_bind(g_icmp_raw, IP_ADDR_ANY);

#if LWIP_IPV6
    g_icmp6_raw = raw_new_ip_type(IPADDR_TYPE_V6, IP6_NEXTH_ICMP6);
    if (!g_icmp6_raw) {
        printf("[net] lwip: raw_new(ICMPv6) failed\n");
        return;
    }
    raw_recv(g_icmp6_raw, edge_lwip_icmp6_recv, 0);
    {
        ip_addr_t any6;
        ip_addr_set_any(1, &any6);
        raw_bind(g_icmp6_raw, &any6);
    }
    g_icmp6_raw->chksum_reqd = 1;
    g_icmp6_raw->chksum_offset = 2;
#endif

    e1000_set_rx_frame_callback(edge_lwip_rx_frame_cb);
    g_ready = 1;
    edge_try_reload_hostname_from_file();
    edge_try_reload_dns_from_resolv_conf();
    printf("[net] lwip: ready ip=10.0.2.15 gw=10.0.2.2\n");
#if LWIP_IPV6
    {
        char ip6buf[48];
        if (ip6addr_ntoa_r(netif_ip6_addr(&g_lwip_netif, 0), ip6buf, sizeof(ip6buf))) {
            printf("[net] lwip: ipv6 ll=%s\n", ip6buf);
        }
    }
#endif
}

void lwip_stack_poll(void) {
    if (!g_ready) return;
    e1000_poll();
    sys_check_timeouts();
}

int lwip_stack_is_ready(void) {
    return g_ready ? 1 : 0;
}

int lwip_stack_send_icmp_echo(uint32_t dst_ip_be, const uint8_t *icmp_payload, uint16_t icmp_len, uint8_t ttl) {
    struct pbuf *p;
    ip_addr_t dst;
    err_t err;
    uint8_t icmp_buf[1600];

    if (!g_ready || !g_icmp_raw || !icmp_payload || icmp_len < 8) {
        return -1;
    }
    if (icmp_len > sizeof(icmp_buf)) return -1;

    p = pbuf_alloc(PBUF_IP, icmp_len, PBUF_RAM);
    if (!p) return -1;

    memcpy(icmp_buf, icmp_payload, icmp_len);
    icmp_buf[2] = 0;
    icmp_buf[3] = 0;
    {
        uint16_t csum = edge_cksum16(icmp_buf, icmp_len);
        icmp_buf[2] = (uint8_t)(csum >> 8);
        icmp_buf[3] = (uint8_t)(csum & 0xFFu);
    }
    if (pbuf_take(p, icmp_buf, icmp_len) != ERR_OK) {
        pbuf_free(p);
        return -1;
    }

    ip_addr_set_zero(&dst);
    ip_2_ip4(&dst)->addr = dst_ip_be;
    if (ttl == 0) ttl = 64;
    g_icmp_raw->ttl = ttl;
    err = raw_sendto(g_icmp_raw, p, &dst);
    pbuf_free(p);
    return err == ERR_OK ? 0 : -1;
}

int lwip_stack_send_icmpv6_echo(const uint8_t dst_ip6[16], const uint8_t *icmp_payload, uint16_t icmp_len, uint8_t hop_limit) {
#if LWIP_IPV6
    struct pbuf *p;
    ip_addr_t dst;
    uint8_t icmp_buf[1600];
    err_t err;

    if (!g_ready || !g_icmp6_raw || !dst_ip6 || !icmp_payload || icmp_len < 8) return -1;
    if (icmp_len > sizeof(icmp_buf)) return -1;

    p = pbuf_alloc(PBUF_IP, icmp_len, PBUF_RAM);
    if (!p) return -1;

    memcpy(icmp_buf, icmp_payload, icmp_len);
    icmp_buf[2] = 0;
    icmp_buf[3] = 0;
    if (pbuf_take(p, icmp_buf, icmp_len) != ERR_OK) {
        pbuf_free(p);
        return -1;
    }

    ip_addr_set_zero_ip6(&dst);
    memcpy(&ip_2_ip6(&dst)->addr[0], dst_ip6, 16);
    if (hop_limit == 0) hop_limit = 64;
    g_icmp6_raw->ttl = hop_limit;
    err = raw_sendto(g_icmp6_raw, p, &dst);
    pbuf_free(p);
    return err == ERR_OK ? 0 : -1;
#else
    (void)dst_ip6;
    (void)icmp_payload;
    (void)icmp_len;
    (void)hop_limit;
    return -1;
#endif
}

int lwip_stack_recv_icmp_reply_for_id(uint16_t id_be, uint8_t *ip_packet_out, uint32_t *ip_packet_len, uint32_t *src_ip_be) {
    if (!g_ready || !ip_packet_len) return -1;

    lwip_stack_poll();

    for (int i = 0; i < EDGE_ICMP_REPLY_Q; ++i) {
        edge_icmp_reply_t *r = &g_reply_q[i];
        if (!r->used) continue;
        if (r->id_be != id_be) continue;

        if (ip_packet_out && *ip_packet_len >= r->ip_len) {
            memcpy(ip_packet_out, r->ip_pkt, r->ip_len);
            *ip_packet_len = r->ip_len;
            if (src_ip_be) *src_ip_be = r->src_ip_be;
        } else {
            *ip_packet_len = r->ip_len;
            return -1;
        }

        r->used = 0;
        return 1;
    }

    return 0;
}

int lwip_stack_recv_icmp_packet(uint8_t *ip_packet_out, uint32_t *ip_packet_len, uint32_t *src_ip_be) {
    if (!g_ready || !ip_packet_len) return -1;

    lwip_stack_poll();

    for (int i = 0; i < EDGE_ICMP_RAW_Q; ++i) {
        edge_icmp_reply_t *r = &g_raw_icmp_q[i];
        if (!r->used) continue;

        if (ip_packet_out && *ip_packet_len >= r->ip_len) {
            memcpy(ip_packet_out, r->ip_pkt, r->ip_len);
            *ip_packet_len = r->ip_len;
            if (src_ip_be) *src_ip_be = r->src_ip_be;
        } else {
            *ip_packet_len = r->ip_len;
            return -1;
        }

        r->used = 0;
        return 1;
    }

    return 0;
}

int lwip_stack_recv_icmpv6_reply_for_id(uint16_t id_be, uint8_t *packet_out, uint32_t *packet_len, uint8_t src_ip6_out[16]) {
#if LWIP_IPV6
    if (!g_ready || !packet_len) return -1;

    lwip_stack_poll();

    for (int i = 0; i < EDGE_ICMP6_REPLY_Q; ++i) {
        edge_icmp6_reply_t *r = &g_reply6_q[i];
        if (!r->used) continue;
        if (r->id_be != id_be) continue;

        if (packet_out && *packet_len >= r->pkt_len) {
            memcpy(packet_out, r->pkt, r->pkt_len);
            *packet_len = r->pkt_len;
            if (src_ip6_out) memcpy(src_ip6_out, r->src_ip6, 16);
        } else {
            *packet_len = r->pkt_len;
            return -1;
        }

        r->used = 0;
        return 1;
    }

    return 0;
#else
    (void)id_be;
    (void)packet_out;
    (void)packet_len;
    (void)src_ip6_out;
    return -1;
#endif
}

int lwip_stack_get_ipv6_addr(uint8_t out[16], int prefer_global) {
#if LWIP_IPV6
    if (!out || !g_ready) return -1;

    for (u8_t i = 0; i < LWIP_IPV6_NUM_ADDRESSES; ++i) {
        const ip6_addr_t *a = netif_ip6_addr(&g_lwip_netif, i);
        u8_t st = netif_ip6_addr_state(&g_lwip_netif, i);
        if (!ip6_addr_isvalid(st) || ip6_addr_isany(a)) continue;
        if (prefer_global && ip6_addr_islinklocal(a)) continue;
        edge_ip6_to_bytes(a, out);
        return 0;
    }

    if (prefer_global) return lwip_stack_get_ipv6_addr(out, 0);
    return -1;
#else
    (void)out;
    (void)prefer_global;
    return -1;
#endif
}

int lwip_stack_get_ipv6_addr_at(int ordinal, uint8_t out[16], uint8_t *prefix_len, uint8_t *scope, uint8_t *flags) {
#if LWIP_IPV6
    int n = 0;
    if (!g_ready || ordinal < 0 || !out) return -1;

    for (u8_t i = 0; i < LWIP_IPV6_NUM_ADDRESSES; ++i) {
        const ip6_addr_t *a = netif_ip6_addr(&g_lwip_netif, i);
        u8_t st = netif_ip6_addr_state(&g_lwip_netif, i);
        if (!ip6_addr_isvalid(st) || ip6_addr_isany(a)) continue;
        if (n++ != ordinal) continue;

        edge_ip6_to_bytes(a, out);
        if (prefix_len) *prefix_len = ip6_addr_isloopback(a) ? 128u : 64u;
        if (scope) {
            if (ip6_addr_isloopback(a)) *scope = 0x10u;
            else if (ip6_addr_islinklocal(a)) *scope = 0x20u;
            else if (ip6_addr_issitelocal(a)) *scope = 0x40u;
            else *scope = 0x00u;
        }
        if (flags) *flags = 0x80u;
        return 0;
    }
    return -1;
#else
    (void)ordinal;
    (void)out;
    (void)prefix_len;
    (void)scope;
    (void)flags;
    return -1;
#endif
}

int lwip_stack_set_hostname(const char *name) {
    char sanitized[65];
    int si = 0;
    int oi = 0;

    if (!name || !name[0]) return -1;
    while (name[si] && oi < (int)sizeof(sanitized) - 1) {
        char c = name[si++];
        if (!edge_valid_hostname_char(c)) continue;
        sanitized[oi++] = c;
    }
    if (oi == 0) return -1;
    sanitized[oi] = 0;
    strcpy(g_hostname, sanitized);
#if LWIP_NETIF_HOSTNAME
    if (g_ready) netif_set_hostname(&g_lwip_netif, g_hostname);
#endif
    return 0;
}

const char *lwip_stack_get_hostname(void) {
    return g_hostname;
}

int lwip_stack_reload_system_config(void) {
    edge_try_reload_hostname_from_file();
    edge_try_reload_dns_from_resolv_conf();
    return 0;
}

void lwip_stack_get_link_stats(uint64_t *rx_packets, uint64_t *rx_bytes, uint64_t *tx_packets, uint64_t *tx_bytes) {
    if (rx_packets) *rx_packets = g_rx_packets;
    if (rx_bytes) *rx_bytes = g_rx_bytes;
    if (tx_packets) *tx_packets = g_tx_packets;
    if (tx_bytes) *tx_bytes = g_tx_bytes;
}
