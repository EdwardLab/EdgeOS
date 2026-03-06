#ifndef EDGEOS_LWIPOPTS_H
#define EDGEOS_LWIPOPTS_H

#define NO_SYS                          1
#define SYS_LIGHTWEIGHT_PROT            0

#define LWIP_SOCKET                     0
#define LWIP_NETCONN                    0
#define LWIP_TCPIP_CORE_LOCKING         0

#define LWIP_IPV4                       1
#define LWIP_IPV6                       1
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_ICMP6                      1
#define LWIP_RAW                        1
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_DNS                        1
#define DNS_MAX_SERVERS                 4
#define LWIP_IGMP                       0
#define LWIP_IPV6_MLD                   1
#define LWIP_IPV6_AUTOCONFIG            1
#define LWIP_ND6_ALLOW_RA_UPDATES       1
#define LWIP_IPV6_DUP_DETECT_ATTEMPTS   0
/* x86_64 pointers don't fit in the IPv6 fragment header helper overlay. */
#define IPV6_FRAG_COPYHEADER            1

#define LWIP_STATS                      0
#define LWIP_NETIF_STATUS_CALLBACK      0
#define LWIP_NETIF_LINK_CALLBACK        0
#define LWIP_NETIF_HOSTNAME             1
#define LWIP_SINGLE_NETIF               1
#define LWIP_HAVE_LOOPIF                0

#define LWIP_NETIF_TX_SINGLE_PBUF       1
#define ETH_PAD_SIZE                    0
#define LWIP_CHKSUM_ALGORITHM           3
#define LWIP_NO_CTYPE_H                 1
#define LWIP_RAND()                     ((u32_t)sys_now())

#define MEM_ALIGNMENT                   8
#define MEM_SIZE                        (64 * 1024)
#define MEMP_NUM_PBUF                   64
#define MEMP_NUM_RAW_PCB                8
#define PBUF_POOL_SIZE                  64
#define PBUF_POOL_BUFSIZE               1600

#define IP_REASSEMBLY                   0
#define IP_FRAG                         0
#define LWIP_ARP_QUEUEING               0

#define LWIP_TIMEVAL_PRIVATE            0

#define LWIP_DBG_MIN_LEVEL              LWIP_DBG_LEVEL_OFF
#define LWIP_DBG_TYPES_ON               0

#endif
