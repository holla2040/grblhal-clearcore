/*
 * ethernetif.c — lwIP 2.1 NO_SYS network interface ("e0") over our GMAC
 * driver, for grblhal-clearcore.
 *
 * Everything here runs in the grblHAL foreground. gmac.c's interrupt handler
 * never touches lwIP, which is what lets lwipopts.h leave
 * SYS_LIGHTWEIGHT_PROT off.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 *
 * Shaped after the skeleton lwIP ships as src/netif/ethernetif.c (BSD-3,
 * Swedish Institute of Computer Science) and Teknic ClearCore-library 1.7.4's
 * port of it (MIT, Copyright (c) 2020 Teknic, Inc.). Changes: descriptor-ring
 * walking moved behind gmac.c's frame API; the receive path is pumped a
 * bounded number of frames per call from the foreground instead of looping
 * until the ring drains; transmit linearizes pbuf chains into a static buffer
 * rather than mem_malloc/mem_free per frame; link state is polled and pushed
 * into lwIP instead of arriving on the PHY interrupt.
 */

/* ETHERNET_ENABLE comes from my_machine.h, which platformio.ini force-includes
   into every translation unit (-include src/my_machine.h). */
#if ETHERNET_ENABLE

#include <string.h>

#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include "lwip/netif.h"
#include "netif/etharp.h"
#include "netif/ethernet.h"

#include "gmac.h"
#include "systick.h"

/* Interface name reported by lwIP: "e0". */
#define IFNAME0 'e'
#define IFNAME1 '0'

/* Frames handed to netif->input per ethernetif_poll() call. The cap keeps a
   burst of traffic from stretching one pass through grblHAL's realtime
   loop; whatever is left stays in the GMAC ring for the next call. */
#define ETHERNETIF_RX_BURST 8

/*
 * Separate staging buffers: lwIP sends from inside netif->input (a TCP
 * segment gets acknowledged while its frame is still being processed), so the
 * receive and transmit paths can be live at the same time.
 */
static uint8_t rx_frame[GMAC_MAX_FRAME_LEN];
static uint8_t tx_frame[GMAC_MAX_FRAME_LEN];

static uint8_t if_mac[ETH_HWADDR_LEN];
static bool mac_known;
static uint32_t rand_state;

/*
 * xorshift32 for LWIP_RAND() (see arch/cc.h). DHCP transaction IDs and TCP
 * initial sequence numbers only need to differ between boots and between
 * boards, not to be unpredictable to an attacker.
 */
uint32_t lwip_rand (void)
{
    uint32_t x = rand_state ? rand_state : 0x1e5f2c9du;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rand_state = x;

    return x;
}

/*
 * lwIP's millisecond time base under NO_SYS. Weak, so the networking glue can
 * override it if it wants a different clock; by default it is our SysTick.
 */
__attribute__((weak)) u32_t sys_now (void)
{
    return (u32_t)millis();
}

/*
 * Adopt a MAC address obtained from an earlier gmac_init(). enet.c brings the
 * GMAC up before the netif exists, so that the address can be reported to
 * grblHAL's network info while the interface is still coming up; telling us
 * the address here keeps ethernetif_init() from initializing the GMAC twice.
 */
void ethernetif_set_mac (const uint8_t mac[6])
{
    memcpy(if_mac, mac, ETH_HWADDR_LEN);
    mac_known = true;
}

/*
 * Bring up the hardware. Called from ethernetif_init().
 */
static void low_level_init (struct netif *netif)
{
    if (!mac_known)
        gmac_init(if_mac);      /* nobody initialized the GMAC for us */

    netif->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(netif->hwaddr, if_mac, ETH_HWADDR_LEN);

    /* Maximum transfer unit: the IP payload, not the frame. */
    netif->mtu = 1500;

    /*
     * Device capabilities. NETIF_FLAG_LINK_UP is deliberately absent: the
     * link is a property of the PHY, and enet.c's link_check task raises and
     * drops it from gmac_link_up(). Setting it here would tell lwIP the wire
     * is live before anyone has looked.
     */
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                   NETIF_FLAG_ETHERNET;

    /* Seed the PRNG from the MAC so two boards on one network diverge. */
    rand_state = ((uint32_t)if_mac[2] << 24) | ((uint32_t)if_mac[3] << 16) |
                 ((uint32_t)if_mac[4] << 8) | if_mac[5];
    rand_state ^= (uint32_t)millis();
}

/*
 * Transmit one frame. The pbuf may be chained.
 *
 * Returns ERR_MEM when the transmit ring is full: lwIP treats that as "try
 * again later" and the TCP timers will re-drive the segment. We do not spin
 * waiting for a descriptor — this runs on the same thread as motion
 * housekeeping.
 */
static err_t low_level_output (struct netif *netif, struct pbuf *p)
{
    const uint8_t *frame;

    LWIP_UNUSED_ARG(netif);

    if (p->tot_len > GMAC_MAX_FRAME_LEN) {
        LINK_STATS_INC(link.lenerr);
        LINK_STATS_INC(link.drop);
        return ERR_IF;
    }

    if (p->next == NULL) {
        /* Unchained: hand the payload straight to the DMA copy. */
        frame = (const uint8_t *)p->payload;
    } else {
        if (pbuf_copy_partial(p, tx_frame, p->tot_len, 0) != p->tot_len) {
            LINK_STATS_INC(link.err);
            LINK_STATS_INC(link.drop);
            return ERR_IF;
        }
        frame = tx_frame;
    }

    if (!gmac_tx_frame(frame, p->tot_len)) {
        LINK_STATS_INC(link.drop);
        return ERR_MEM;
    }

    LINK_STATS_INC(link.xmit);

    return ERR_OK;
}

/*
 * Move received frames into lwIP. Call this regularly from the grblHAL
 * foreground, alongside sys_check_timeouts(). Never call it from an
 * interrupt.
 *
 * Link state is not touched here: gmac_link_up() costs MDIO transactions and
 * belongs on a slow timer, which is where enet.c's link_check task drives it.
 */
void ethernetif_poll (struct netif *netif)
{
    uint8_t n;

    for (n = 0; n < ETHERNETIF_RX_BURST; n++) {

        struct pbuf *p;
        uint16_t len = gmac_rx_frame(rx_frame, sizeof(rx_frame));

        if (len == 0)
            break;

        p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);

        if (p == NULL) {
            /* Out of pool buffers. The frame is already out of the GMAC ring
               and is dropped here; stop so the pool has a chance to refill. */
            LINK_STATS_INC(link.memerr);
            LINK_STATS_INC(link.drop);
            break;
        }

        if (pbuf_take(p, rx_frame, len) != ERR_OK) {
            pbuf_free(p);
            LINK_STATS_INC(link.drop);
            break;
        }

        LINK_STATS_INC(link.recv);

        if (netif->input(p, netif) != ERR_OK)
            pbuf_free(p);
    }
}

/*
 * netif_add() initializer. Pass ethernet_input as netif_add()'s input
 * function so ARP and IP demultiplexing happen in lwIP:
 *
 *   netif_add(&netif, &ip, &mask, &gw, NULL, ethernetif_init, ethernet_input);
 */
err_t ethernetif_init (struct netif *netif)
{
    LWIP_ASSERT("netif != NULL", (netif != NULL));

#if LWIP_NETIF_HOSTNAME
    netif->hostname = "grblHAL";
#endif

    netif->name[0] = IFNAME0;
    netif->name[1] = IFNAME1;

    netif->output = etharp_output;
    netif->linkoutput = low_level_output;

    low_level_init(netif);

    return ERR_OK;
}

#endif /* ETHERNET_ENABLE */
