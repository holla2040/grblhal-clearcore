/*
 * lwipopts.h — lwIP 2.1 configuration for grblhal-clearcore.
 * NO_SYS raw-API mode: ALL lwIP calls happen in the grbl foreground
 * (task_add_systick pump); the GMAC ISR never touches lwIP, so no
 * lightweight protection is needed. Telnet-first scope: TCP + ARP +
 * DHCP, no sockets/netconn, no IPv6.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                     1
#define SYS_LIGHTWEIGHT_PROT       0
#define LWIP_NETCONN               0
#define LWIP_SOCKET                0
#define LWIP_IGMP                  1        /* mDNS multicast group */
#define LWIP_ICMP                  1
#define LWIP_RAW                   0
#define LWIP_DNS                   0
#define LWIP_IPV4                  1
#define LWIP_IPV6                  0
#define LWIP_ARP                   1
#define LWIP_DHCP                  1
#define LWIP_AUTOIP                0
#define LWIP_UDP                   1        /* DHCP needs it */
#define LWIP_TCP                   1
#define LWIP_STATS                 0
#define LWIP_NETIF_HOSTNAME        1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK   1
/* mDNS responder: answers <hostname>.local. The ext status callback lets
   mdns re-probe/announce on its own when DHCP delivers or changes the
   address, so enet.c registers the netif once and never has to babysit. */
#define LWIP_NETIF_EXT_STATUS_CALLBACK 1
#define LWIP_MDNS_RESPONDER        1
#define LWIP_NUM_NETIF_CLIENT_DATA 1        /* mDNS claims one slot */
#define LWIP_CHKSUM_ALGORITHM      3

#define MEM_ALIGNMENT              4
#define MEM_SIZE                   (16 * 1024)
#define MEMP_NUM_PBUF              16
#define MEMP_NUM_TCP_PCB           8
#define MEMP_NUM_TCP_PCB_LISTEN    4
#define MEMP_NUM_TCP_SEG           32
#define MEMP_NUM_SYS_TIMEOUT       14       /* +IGMP tmr, +mDNS probe/announce */
#define PBUF_POOL_SIZE             12
#define PBUF_POOL_BUFSIZE          1536

#define TCP_MSS                    1460
#define TCP_WND                    (4 * TCP_MSS)
#define TCP_SND_BUF                (4 * TCP_MSS)
#define TCP_SND_QUEUELEN           (2 * TCP_SND_BUF / TCP_MSS)
#define TCP_QUEUE_OOSEQ            0
#define LWIP_TCP_KEEPALIVE         1

/* Options for the networking plugin's VFS-based httpd */
#define LWIP_HTTPD_DYNAMIC_HEADERS      1
#define LWIP_HTTPD_DYNAMIC_FILE_READ    1
#define LWIP_HTTPD_SUPPORT_V09          0
#define LWIP_HTTPD_SUPPORT_11_KEEPALIVE 1
#define LWIP_HTTPD_SUPPORT_POST         1

#define LWIP_NETIF_API             0

#define LWIP_PLATFORM_BYTESWAP     0
#define BYTE_ORDER                 LITTLE_ENDIAN

#endif /* LWIPOPTS_H */
